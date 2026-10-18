// In-memory ELF symbol resolution for already-mapped libraries.
//
// dlopen()/dl_iterate_phdr only see the caller's linker namespace, which for
// this payload (loaded by the Zygisk companion) excludes both the app's
// classloader namespace and /system's libraries. /proc/self/maps lists every
// mapping in the process regardless of namespace, so the load base plus the
// in-memory .dynamic table are enough to walk .dynsym directly.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <elf.h>
#include <link.h>   // ElfW

#include "vdhs_sym.h"

// Find an already-mapped shared library by substring and return its load base
// (first file-offset-0 readable mapping) plus its path.
int vdhs_find_mapped_base(const char* needle, uintptr_t* out_base, char* path, size_t cap) {
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return 0;
    char line[512];
    int found = 0;
    while (fgets(line, sizeof line, f)) {
        if (!strstr(line, needle)) continue;
        unsigned long start = 0, end = 0, off = 0;
        char perms[8] = {0};
        if (sscanf(line, "%lx-%lx %7s %lx", &start, &end, perms, &off) != 4) continue;
        if (off != 0 || perms[0] != 'r') continue;
        *out_base = (uintptr_t)start;
        char* slash = strrchr(line, '/');
        if (slash) {
            size_t n = strlen(slash);
            while (n > 0 && (slash[n - 1] == '\n' || slash[n - 1] == '\r')) n--;
            if (n > 0 && n < cap) { memcpy(path, slash, n); path[n] = '\0'; }
        }
        found = 1;
        break;
    }
    fclose(f);
    return found;
}

// Symbol count from a SysV or GNU hash table (both give a tight upper bound on
// .dynsym entries). Used because .dynsym has no explicit length in ELF.
static size_t gnu_symcount(const uint32_t* gh) {
    const uint32_t nbuckets = gh[0];
    const uint32_t symoffset = gh[1];
    const uint32_t bloom_size = gh[2];
    const uint32_t* buckets = gh + 4 + bloom_size * (uint32_t)(sizeof(uintptr_t) / 4);
    if (nbuckets == 0) return symoffset;
    uint32_t last = 0;
    for (uint32_t i = 0; i < nbuckets; i++) if (buckets[i] > last) last = buckets[i];
    if (last < symoffset) return symoffset;
    const uint32_t* chain = buckets + nbuckets;   // chain[k] belongs to symbol symoffset+k
    uint32_t idx = last;
    while (!(chain[idx - symoffset] & 1u)) idx++;
    return (size_t)idx + 1;
}

// Walk the in-memory ELF loaded at `base` and fill in the requested dynamic
// symbols. Names are matched exactly; entries of `out` that are already set are
// counted but left alone, so this can be called repeatedly to fill gaps.
static int resolve_in_elf(uintptr_t base, const char* const* names, void** out, int count) {
    const ElfW(Ehdr)* eh = (const ElfW(Ehdr)*)base;
    if (eh->e_ident[EI_MAG0] != ELFMAG0 || eh->e_ident[EI_MAG1] != ELFMAG1 ||
        eh->e_ident[EI_MAG2] != ELFMAG2 || eh->e_ident[EI_MAG3] != ELFMAG3 ||
        eh->e_ident[EI_CLASS] != ELFCLASS64)
        return 0;
    if (eh->e_phoff == 0 || eh->e_phoff > 0x10000) return 0;
    if (eh->e_phnum == 0 || eh->e_phnum > 64 || eh->e_phentsize < sizeof(ElfW(Phdr))) return 0;

    const ElfW(Phdr)* ph = (const ElfW(Phdr)*)(base + eh->e_phoff);
    const ElfW(Dyn)* dyn = NULL;
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type == PT_DYNAMIC) { dyn = (const ElfW(Dyn)*)(base + ph[i].p_vaddr); break; }
    }
    if (!dyn) return 0;

    const ElfW(Sym)* symtab = NULL;
    const char* strtab = NULL;
    const uint32_t* gnu = NULL;
    const uint32_t* hash = NULL;
    for (const ElfW(Dyn)* d = dyn; d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
            case DT_SYMTAB:   symtab = (const ElfW(Sym)*)(base + d->d_un.d_ptr); break;
            case DT_STRTAB:   strtab = (const char*)(base + d->d_un.d_ptr); break;
            case DT_GNU_HASH: gnu = (const uint32_t*)(base + d->d_un.d_ptr); break;
            case DT_HASH:     hash = (const uint32_t*)(base + d->d_un.d_ptr); break;
            default: break;
        }
    }
    if (!symtab || !strtab) return 0;

    size_t nsyms = 0;
    if (gnu) nsyms = gnu_symcount(gnu);
    else if (hash) nsyms = (size_t)hash[1];
    if (nsyms == 0 || nsyms > (1u << 20)) return 0;

    int got = 0;
    for (int j = 0; j < count; j++) {
        if (out[j]) { got++; continue; }
        for (size_t i = 0; i < nsyms; i++) {
            if (symtab[i].st_name >= 0x100000) continue;
            const char* nm = strtab + symtab[i].st_name;
            if (strcmp(nm, names[j]) == 0) {
                out[j] = (void*)(base + symtab[i].st_value);
                got++;
                break;
            }
        }
    }
    return got;
}

// Libraries inside an APK (extractNativeLibs=false) are mapped straight out of
// the container, so /proc/self/maps names the .apk and never the .so. Those
// mappings still begin with the embedded ELF header, so scan for it.
static int is_elf_header(uintptr_t start, unsigned long len) {
    if (len < sizeof(ElfW(Ehdr))) return 0;
    const ElfW(Ehdr)* eh = (const ElfW(Ehdr)*)start;
    return eh->e_ident[EI_MAG0] == ELFMAG0 && eh->e_ident[EI_MAG1] == ELFMAG1 &&
           eh->e_ident[EI_MAG2] == ELFMAG2 && eh->e_ident[EI_MAG3] == ELFMAG3 &&
           eh->e_ident[EI_CLASS] == ELFCLASS64;
}

int vdhs_collect_elf_bases(uintptr_t* out, int max) {
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return 0;
    char line[512];
    int n = 0;
    while (fgets(line, sizeof line, f) && n < max) {
        unsigned long start = 0, end = 0, off = 0;
        char perms[8] = {0};
        if (sscanf(line, "%lx-%lx %7s %lx", &start, &end, perms, &off) != 4) continue;
        if (perms[0] != 'r') continue;
        if (!strstr(line, ".apk") && !strstr(line, "/dev/ashmem/")) continue;
        if (!is_elf_header((uintptr_t)start, end - start)) continue;
        int dup = 0;
        for (int i = 0; i < n; i++) if (out[i] == (uintptr_t)start) dup = 1;
        if (!dup) out[n++] = (uintptr_t)start;
    }
    fclose(f);
    return n;
}

static int resolve_in_apk_elfs(const char* const* names, void** out, int count) {
    uintptr_t bases[64];
    int nb = vdhs_collect_elf_bases(bases, 64);
    int got = 0;
    for (int i = 0; i < nb; i++) {
        int g = resolve_in_elf(bases[i], names, out, count);
        if (g > got) got = g;
        if (got >= count) break;
    }
    return got;
}

// Resolve dynamic symbols of an already-mapped library by walking its in-memory
// ELF. NULL entries in `out` are left untouched so a caller can request a mix of
// required and optional symbols.
int vdhs_resolve_mapped_symbols(const char* needle, const char* const* names,
                                void** out, int count) {
    int got = 0;
    uintptr_t base = 0;
    char path[512] = {0};
    if (needle && vdhs_find_mapped_base(needle, &base, path, sizeof path))
        got = resolve_in_elf(base, names, out, count);
    if (got < count) {
        int g = resolve_in_apk_elfs(names, out, count);
        if (g > got) got = g;
    }
    return got;
}

// Single-symbol variant, exported for vdhs_mark.c (weak reference there, so a
// build without this file still links).
__attribute__((visibility("default")))
void* vdhs_resolve_mapped_symbol(const char* lib, const char* name) {
    const char* names[1] = { name };
    void* out[1] = { NULL };
    if (vdhs_resolve_mapped_symbols(lib, names, out, 1) <= 0) return NULL;
    return out[0];
}
