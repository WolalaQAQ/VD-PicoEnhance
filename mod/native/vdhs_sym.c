// Resolve only symbols belonging to one already-mapped shared object.
#include <string.h>

#include "vdhs_sym.h"
#include "xdl.h"

struct sym_query {
    const char* lib;
    const char* const* names;
    void** out;
    int count;
    int got;
};

static int matches_library(const char* path, const char* lib) {
    size_t n = strlen(path), k = strlen(lib);
    if (k == 0 || n < k || strcmp(path + n - k, lib) != 0) return 0;
    // A basename/relative suffix must start at a path-component boundary.
    return n == k || lib[0] == '/' || path[n - k - 1] == '/';
}

static int resolve_in_object(struct dl_phdr_info* info, size_t size, void* data) {
    (void)size;
    struct sym_query* q = (struct sym_query*)data;
    if (!info->dlpi_name || !matches_library(info->dlpi_name, q->lib)) return 0;

    // xdl_open("/lib/arm64/...", XDL_DEFAULT) would require an exact absolute
    // path, not the suffix used by our loader callers. Open the selected phdr
    // directly instead: xdl_open2 never dlopens a new library.
    void* handle = xdl_open2(info);
    if (!handle) return 1;
    for (int i = 0; i < q->count; i++) {
        if (!q->out[i]) q->out[i] = xdl_sym(handle, q->names[i], NULL);
        if (q->out[i]) q->got++;
    }
    xdl_close(handle);
    // Even a partial/empty result belongs to this object, not the next one.
    return 1;
}

int vdhs_resolve_mapped_symbols(const char* lib, const char* const* names,
                                void** out, int count) {
    struct sym_query q = { lib, names, out, count, 0 };
    xdl_iterate_phdr(resolve_in_object, &q, XDL_DEFAULT);
    return q.got;
}

// vdhs_mark.c keeps its weak declaration, so standalone builds need not link us.
__attribute__((visibility("default")))
void* vdhs_resolve_mapped_symbol(const char* lib, const char* name) {
    const char* names[1] = { name };
    void* out[1] = { NULL };
    vdhs_resolve_mapped_symbols(lib, names, out, 1);
    return out[0];
}
