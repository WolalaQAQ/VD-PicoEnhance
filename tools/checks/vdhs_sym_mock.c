// Host-only xDL double. This tests selection, not Android linker namespaces.
#include "xdl.h"
#include <string.h>

struct mock_library {
    struct dl_phdr_info info;
    char path[1024];
    char names[8][128];
    void* values[8];
    int symbols;
    int can_open;
};

static struct mock_library libraries[8];
static int library_count;
__declspec(dllexport) int sym_mock_opens;
__declspec(dllexport) int sym_mock_closes;
__declspec(dllexport) int sym_mock_queries;
__declspec(dllexport) int sym_mock_iterations;
__declspec(dllexport) int sym_mock_flags;

// No host CRT/SDK is required: clang builds a no-entry, no-import test DLL.
size_t strlen(const char* s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

int strcmp(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

void* memset(void* destination, int value, size_t count) {
    unsigned char* out = (unsigned char*)destination;
    for (size_t i = 0; i < count; i++) out[i] = (unsigned char)value;
    return destination;
}

static void copy_string(char* out, const char* in) {
    while ((*out++ = *in++)) {}
}

__declspec(dllexport) void sym_mock_reset(void) {
    library_count = 0;
    sym_mock_opens = sym_mock_closes = sym_mock_queries = 0;
    sym_mock_iterations = 0;
    sym_mock_flags = -1;
}

__declspec(dllexport) int sym_mock_add(const char* path, int can_open) {
    int index = library_count++;
    struct mock_library* lib = &libraries[index];
    copy_string(lib->path, path);
    lib->info.dlpi_name = lib->path;
    lib->symbols = 0;
    lib->can_open = can_open;
    return index;
}

__declspec(dllexport) void sym_mock_symbol(int index, const char* name,
                                          unsigned long long address) {
    struct mock_library* lib = &libraries[index];
    int symbol = lib->symbols++;
    copy_string(lib->names[symbol], name);
    lib->values[symbol] = (void*)address;
}

int xdl_iterate_phdr(int (*callback)(struct dl_phdr_info*, size_t, void*),
                     void* data, int flags) {
    sym_mock_iterations++;
    sym_mock_flags = flags;
    for (int i = 0; i < library_count; i++) {
        int r = callback(&libraries[i].info, sizeof(struct dl_phdr_info), data);
        if (r) return r;
    }
    return 0;
}

void* xdl_open2(struct dl_phdr_info* info) {
    sym_mock_opens++;
    for (int i = 0; i < library_count; i++) {
        if (info == &libraries[i].info)
            return libraries[i].can_open ? &libraries[i] : NULL;
    }
    return NULL;
}

void* xdl_sym(void* handle, const char* name, size_t* symbol_size) {
    (void)symbol_size;
    struct mock_library* lib = (struct mock_library*)handle;
    sym_mock_queries++;
    for (int i = 0; i < lib->symbols; i++) {
        if (strcmp(lib->names[i], name) == 0) return lib->values[i];
    }
    return NULL;
}

void* xdl_close(void* handle) {
    (void)handle;
    sym_mock_closes++;
    return NULL;
}
