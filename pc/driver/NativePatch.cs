// The shipped capsule contains our code only. Original unwind/relocation data
// is copied from the user's verified VD image on that user's computer.
using System;
using System.Collections.Generic;
using System.IO;
using System.Security.Cryptography;
using System.Text;

namespace VdPicoEnhance {
public static class NativePatch {
    public const string OriginalSha256 = "ad3c99c7f7346613d4c7106fb94476be5859ca17a637a11cfc156184d466f36f";
    public const uint CodeRva = 0xAA000;
    public const uint Delta = CodeRva - 0x1000;
    public const ulong ImageBase = 0x180000000;
    static uint Align(uint x, uint alignment) { return (x + alignment - 1) / alignment * alignment; }
    static ushort U16(byte[] b, int p) { return BitConverter.ToUInt16(b, p); }
    static uint U32(byte[] b, int p) { return BitConverter.ToUInt32(b, p); }
    static void Put(byte[] b, int p, uint v) { Buffer.BlockCopy(BitConverter.GetBytes(v), 0, b, p, 4); }
    static void Put64(byte[] b, int p, ulong v) { Buffer.BlockCopy(BitConverter.GetBytes(v), 0, b, p, 8); }
    static byte[] Slice(byte[] b, int p, int count) { var result = new byte[count]; Buffer.BlockCopy(b, p, result, 0, count); return result; }
    static void Require(bool condition, string text) { if (!condition) throw new InvalidDataException(text); }
    public static string Hash(byte[] bytes) {
        using (var sha = SHA256.Create()) return BitConverter.ToString(sha.ComputeHash(bytes)).Replace("-", "").ToLowerInvariant();
    }
    public sealed class Section {
        public string Name;
        public uint Rva, Size, Raw, RawSize;
        public int Header;
    }
    public sealed class Pe {
        public byte[] Bytes;
        public int Nt, Optional, SectionTable;
        public List<Section> Sections = new List<Section>();
        public Pe(byte[] bytes) {
            Bytes = bytes; Nt = (int)U32(bytes, 0x3c); Optional = Nt + 24;
            Require(U32(bytes, Nt) == 0x4550 && U16(bytes, Nt + 4) == 0x8664 && U16(bytes, Optional) == 0x20b, "Expected an x64 PE32+ image.");
            SectionTable = Optional + U16(bytes, Nt + 20);
            for (int i = 0; i < U16(bytes, Nt + 6); ++i) {
                int p = SectionTable + i * 40;
                Sections.Add(new Section { Name = Encoding.ASCII.GetString(bytes, p, 8).TrimEnd('\0'), Size = U32(bytes, p + 8), Rva = U32(bytes, p + 12), RawSize = U32(bytes, p + 16), Raw = U32(bytes, p + 20), Header = p });
            }
        }
        public int Offset(uint rva) {
            foreach (var s in Sections) if (rva >= s.Rva && rva < s.Rva + Math.Max(s.Size, s.RawSize)) return checked((int)(s.Raw + rva - s.Rva));
            if (rva < U32(Bytes, Optional + 60)) return (int)rva;
            throw new InvalidDataException("RVA is outside image sections.");
        }
        public int Directory(int index) { return Optional + 112 + index * 8; }
        public byte[] DirectoryBytes(int index) { int p = Directory(index); return Slice(Bytes, Offset(U32(Bytes, p)), (int)U32(Bytes, p + 4)); }
        public Dictionary<string, uint> Exports() {
            int p = Offset(U32(Bytes, Directory(0)));
            uint count = U32(Bytes, p + 24);
            int functions = Offset(U32(Bytes, p + 28)), names = Offset(U32(Bytes, p + 32)), ordinals = Offset(U32(Bytes, p + 36));
            var result = new Dictionary<string, uint>();
            for (int i = 0; i < count; ++i) {
                int start = Offset(U32(Bytes, names + 4 * i)), end = start;
                while (Bytes[end] != 0) ++end;
                result.Add(Encoding.ASCII.GetString(Bytes, start, end - start), U32(Bytes, functions + U16(Bytes, ordinals + 2 * i) * 4));
            }
            return result;
        }
    }
    public static Dictionary<string, uint> Exports(byte[] capsule) { return new Pe(capsule).Exports(); }
    public static byte[] Apply(byte[] original, byte[] capsule) {
        Require(Hash(original) == OriginalSha256, "Unsupported or already modified VD driver. Original file was not changed.");
        var old = new Pe(original); var payload = new Pe(capsule);
        Require(BitConverter.ToUInt64(original, old.Optional + 24) == ImageBase && BitConverter.ToUInt64(capsule, payload.Optional + 24) == ImageBase + Delta, "Unexpected image base.");
        Require(U32(capsule, payload.Directory(1)) == 0 && U32(capsule, payload.Optional + 16) == 0, "Capsule must not import libraries or contain DllMain.");
        Section text = null, data = null;
        foreach (var s in payload.Sections) {
            if (s.Name == ".text") text = s;
            else if (s.Name == ".data") data = s;
            else Require(s.Name == ".reloc", "Unexpected capsule section.");
        }
        Require(text != null && data != null && text.Rva == 0x1000, "Missing capsule code/data.");
        var symbols = payload.Exports();
        byte[] code = Slice(capsule, (int)text.Raw, (int)text.Size);
        int reserve = (int)(symbols["vd_native_runtime_table"] - text.Rva);
        for (int i = 0; i < 32768; ++i) Require(code[reserve + i] == 0, "Capsule runtime reserve is not empty.");
        var unwind = new MemoryStream();
        byte[] oldUnwind = old.DirectoryBytes(3); unwind.Write(oldUnwind, 0, oldUnwind.Length);
        byte[] added = payload.DirectoryBytes(3);
        using (var writer = new BinaryWriter(unwind, Encoding.ASCII, true)) {
            for (int p = 0; p < added.Length; p += 12) {
                Require((capsule[payload.Offset(U32(added, p + 8))] >> 3) == 0, "Unsupported chained/handler unwind record.");
                writer.Write(U32(added, p) + Delta); writer.Write(U32(added, p + 4) + Delta); writer.Write(U32(added, p + 8) + Delta);
            }
        }
        byte[] runtime = unwind.ToArray();
        Buffer.BlockCopy(runtime, 0, code, reserve, runtime.Length);
        var reloc = new MemoryStream();
        byte[] oldReloc = old.DirectoryBytes(5); reloc.Write(oldReloc, 0, oldReloc.Length);
        var pages = new SortedDictionary<uint, List<ushort>>();
        byte[] extraReloc = payload.DirectoryBytes(5);
        for (int p = 0; p < extraReloc.Length; ) {
            uint page = U32(extraReloc, p), size = U32(extraReloc, p + 4);
            Require(size >= 8 && p + size <= extraReloc.Length, "Invalid relocation block.");
            for (int i = p + 8; i < p + size; i += 2) {
                ushort word = U16(extraReloc, i); if ((word >> 12) == 0) continue;
                Require((word >> 12) == 10, "Expected DIR64 relocation.");
                uint rva = page + (uint)(word & 4095) + Delta;
                Require(rva >= CodeRva && rva + 8 <= CodeRva + code.Length, "Capsule pointer is outside read-only code.");
                uint destination = rva & ~4095u;
                if (!pages.ContainsKey(destination)) pages.Add(destination, new List<ushort>());
                pages[destination].Add((ushort)(0xa000 | (rva & 4095)));
            }
            p += (int)size;
        }
        using (var writer = new BinaryWriter(reloc, Encoding.ASCII, true)) {
            foreach (var page in pages) {
                if (page.Value.Count % 2 != 0) page.Value.Add(0);
                writer.Write(page.Key); writer.Write((uint)(8 + page.Value.Count * 2));
                foreach (var word in page.Value) writer.Write(word);
            }
        }
        byte[] relocations = reloc.ToArray();
        int relocOffset = (int)Align((uint)(reserve + runtime.Length), 4);
        Require(relocOffset + relocations.Length <= reserve + 32768, "Runtime reserve overflow.");
        Buffer.BlockCopy(relocations, 0, code, relocOffset, relocations.Length);
        byte[] initialized = new byte[data.Size];
        Buffer.BlockCopy(capsule, (int)data.Raw, initialized, 0, (int)Math.Min(data.Size, data.RawSize));
        uint fileAlignment = U32(original, old.Optional + 36), sectionAlignment = U32(original, old.Optional + 32);
        uint endRva = 0;
        foreach (var s in old.Sections) endRva = Math.Max(endRva, s.Rva + s.Size);
        Require(Align(endRva, sectionAlignment) == CodeRva, "Unexpected VD section layout.");
        int sectionHeader = old.SectionTable + old.Sections.Count * 40;
        Require(sectionHeader + 80 <= U32(original, old.Optional + 60), "No room for two section headers.");
        uint codeRaw = Align((uint)original.Length, fileAlignment), codeSize = Align((uint)code.Length, fileAlignment);
        uint dataRaw = codeRaw + codeSize, dataSize = Align((uint)initialized.Length, fileAlignment);
        uint dataRva = data.Rva + Delta;
        Require(CodeRva + code.Length <= dataRva, "Capsule sections overlap.");
        byte[] result = new byte[dataRaw + dataSize];
        Buffer.BlockCopy(original, 0, result, 0, original.Length);
        Buffer.BlockCopy(code, 0, result, (int)codeRaw, code.Length);
        Buffer.BlockCopy(initialized, 0, result, (int)dataRaw, initialized.Length);
        SectionHeader(result, sectionHeader, ".vdbatt", (uint)code.Length, CodeRva, codeSize, codeRaw, 0x60000020);
        SectionHeader(result, sectionHeader + 40, ".vdbdat", (uint)initialized.Length, dataRva, dataSize, dataRaw, 0xc0000040);
        Buffer.BlockCopy(BitConverter.GetBytes((ushort)(old.Sections.Count + 2)), 0, result, old.Nt + 6, 2);
        Put(result, old.Optional + 4, U32(original, old.Optional + 4) + codeSize);
        Put(result, old.Optional + 8, U32(original, old.Optional + 8) + dataSize);
        Put(result, old.Optional + 56, Align(dataRva + (uint)initialized.Length, sectionAlignment));
        Put(result, old.Directory(3), CodeRva + (uint)reserve); Put(result, old.Directory(3) + 4, (uint)runtime.Length);
        Put(result, old.Directory(5), CodeRva + (uint)relocOffset); Put(result, old.Directory(5) + 4, (uint)relocations.Length);
        Put64(result, old.Directory(4), 0); // The patched image is unsigned.
        // The base patch publishes battery only. Knuckles switching is an
        // independently installed provider, not hooks in the VD DLL.
        uint[] slots = {0x7fd70};
        string[] wrappers = {"vd_native_run_frame"};
        for (int i = 0; i < slots.Length; ++i) Put64(result, old.Offset(slots[i]), ImageBase + symbols[wrappers[i]] + Delta);
        Put(result, old.Optional + 64, 0); Put(result, old.Optional + 64, Checksum(result));
        return result;
    }
    static void SectionHeader(byte[] b, int p, string name, uint size, uint rva, uint rawSize, uint raw, uint flags) {
        Array.Clear(b, p, 40); byte[] text = Encoding.ASCII.GetBytes(name); Buffer.BlockCopy(text, 0, b, p, text.Length);
        Put(b, p + 8, size); Put(b, p + 12, rva); Put(b, p + 16, rawSize); Put(b, p + 20, raw); Put(b, p + 36, flags);
    }
    static uint Checksum(byte[] bytes) {
        ulong sum = 0;
        for (int i = 0; i < bytes.Length; i += 2) {
            sum += bytes[i] + (i + 1 < bytes.Length ? (uint)bytes[i + 1] << 8 : 0);
            sum = (sum & 0xffff) + (sum >> 16);
        }
        sum = (sum & 0xffff) + (sum >> 16);
        return (uint)(sum + (ulong)bytes.Length);
    }
}
}
