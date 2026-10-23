#ifndef MV_TRIAL_CAPTURE_H
#define MV_TRIAL_CAPTURE_H
#include <stdint.h>
#include <stdbool.h>
#include "input_probe.h"

typedef struct {
    uint64_t create_calls, update_calls, create_failures, update_failures;
    uint64_t first_callback_ms, trackpad_creates;
    uint64_t public_create_calls, public_update_calls;
} MvCaptureStats;

#ifdef __cplusplus
extern "C" {
#endif
__declspec(dllexport) MvProbe *mv_trial_probe(void);
__declspec(dllexport) void mv_trial_capture_stats(MvCaptureStats *out);
__declspec(dllexport) void mv_trial_set_targets(uintptr_t create_target, uintptr_t update_target);
__declspec(dllexport) int32_t mv_trial_create(void *self, uint64_t container, const char *path, uint64_t *out);
__declspec(dllexport) int32_t mv_trial_update(void *self, uint64_t component, bool value, double offset);
#ifdef __cplusplus
}
#endif
#endif
