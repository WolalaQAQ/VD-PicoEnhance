#ifndef MV_TRIAL_RUNTIME_H
#define MV_TRIAL_RUNTIME_H
#include "native_api.h"

#ifdef __cplusplus
extern "C" {
#endif
int mv_runtime_start(const MvNativeApi *api, const wchar_t *phase_path, void *driver_context);
void mv_runtime_stop(void);
uint32_t mv_runtime_thread(void *unused);
/* C++ SDK bridge, replaced by owned stubs in lifecycle tests. */
int mv_diag_prepare(void *driver_context, const MvNativeApi *api);
void mv_diag_tick(uint64_t now_ms, uint64_t sequence, uint32_t phase);
void mv_diag_shutdown(void);
#ifdef __cplusplus
}
#endif
#endif
