#:package Mono.Cecil@0.11.6
// Bumps the MVID of a managed assembly.
//
// Used to turn a patched Xenko.OpenXR.dll into the redirect artifact. Why:
// the official APK ships an AOT image (lib/arm64-v8a/libaot-Xenko.OpenXR.dll.so).
// Mono's AOT loader checks the assembly GUID/MVID stored in the image; a patch
// that keeps the original MVID keeps matching the image. For the redirect
// backend we want the opposite: a different MVID makes the AOT image be
// rejected, forcing Mono to JIT the assembly so the IL patch actually runs.
//
// Usage:
//   dotnet run mod/tools/bump_mvid.cs -- <in.dll> <out.dll>
using System;
using System.IO;
using Mono.Cecil;

string inPath = args.Length > 0 ? args[0] : throw new Exception("usage: bump_mvid.cs <in.dll> <out.dll>");
string outPath = args.Length > 1 ? args[1] : throw new Exception("usage: bump_mvid.cs <in.dll> <out.dll>");

var asm = AssemblyDefinition.ReadAssembly(inPath);
var oldMvid = asm.MainModule.Mvid;
asm.MainModule.Mvid = Guid.NewGuid();
asm.Write(outPath);
Console.WriteLine($"MVID {oldMvid} -> {asm.MainModule.Mvid}");
Console.WriteLine($"wrote {outPath} ({new FileInfo(outPath).Length} bytes)");
