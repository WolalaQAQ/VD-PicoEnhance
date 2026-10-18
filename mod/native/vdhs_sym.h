#ifndef VDHS_SYM_H
#define VDHS_SYM_H

#ifdef __cplusplus
extern "C" {
#endif

// Select the first mapped object with this exact basename or path suffix
// (including APK !/lib/... paths). Path selectors never fall back to a basename.
// All new results in a batch come from that one object. Existing non-NULL out
// entries are preserved and counted after opening it; no match/open returns 0.
// No library is loaded or retained. Results remain valid only while the caller
// keeps the selected library mapped; each call selects it afresh.
int vdhs_resolve_mapped_symbols(const char* lib, const char* const* names,
                                void** out, int count);
void* vdhs_resolve_mapped_symbol(const char* lib, const char* name);

#ifdef __cplusplus
}
#endif

#endif
