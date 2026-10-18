#ifndef VDHS_SYM_H
#define VDHS_SYM_H

#include <stddef.h>
#include <stdint.h>

// Locate an already-mapped shared library by a substring of its path and return
// the load base of its first mapping.
int vdhs_find_mapped_base(const char* needle, uintptr_t* out_base, char* path, size_t cap);

// Resolve dynamic symbols of an already-mapped library by exact name, walking
// its in-memory ELF (no linker namespace involved). Non-NULL `out` entries are
// treated as already resolved; the return value counts resolved symbols.
int vdhs_resolve_mapped_symbols(const char* needle, const char* const* names,
                                void** out, int count);

// Single-symbol convenience wrapper.
void* vdhs_resolve_mapped_symbol(const char* lib, const char* name);

// Collect the load bases of every ELF image in the process that has no usable
// path in /proc/self/maps (APK-embedded libraries). Returns the number stored.
int vdhs_collect_elf_bases(uintptr_t* out, int max);

#endif
