#include "capture.h"
#include <intrin.h>

#ifndef VD_ENTRY_RVA
#error VD_ENTRY_RVA is required
#endif
extern void vd_native_run_frame(void *);
typedef int32_t (*Create)(void *, uint64_t, const char *, uint64_t *);
typedef int32_t (*Update)(void *, uint64_t, bool, double);
typedef uint64_t (*Clock)(void);

static MvProbe probe;
static volatile long long creates, updates, create_errors, update_errors, first_callback, trackpad_creates;
static volatile long long expected_create, expected_update, public_creates, public_updates;
static uintptr_t base(void) { return (uintptr_t)&vd_native_run_frame - VD_ENTRY_RVA; }
static uint64_t receipt(void) { return (*(Clock *)(base() + 0x7D1B8))(); }
static int trackpad(const char *p) {
    const char *expected = MV_TRACKPAD_TOUCH;
    if (!p) return 0;
    while (*p && *p == *expected) { ++p; ++expected; }
    return *p == *expected;
}
static void observed(uint64_t now) {
    _InterlockedCompareExchange64(&first_callback, (long long)now, 0);
}

__declspec(dllexport) MvProbe *mv_trial_probe(void) { return &probe; }
__declspec(dllexport) void mv_trial_set_targets(uintptr_t create_target, uintptr_t update_target) {
    _InterlockedExchange64(&expected_create, (long long)create_target);
    _InterlockedExchange64(&expected_update, (long long)update_target);
}
__declspec(dllexport) void mv_trial_capture_stats(MvCaptureStats *out) {
    out->create_calls = (uint64_t)_InterlockedCompareExchange64(&creates, 0, 0);
    out->update_calls = (uint64_t)_InterlockedCompareExchange64(&updates, 0, 0);
    out->create_failures = (uint64_t)_InterlockedCompareExchange64(&create_errors, 0, 0);
    out->update_failures = (uint64_t)_InterlockedCompareExchange64(&update_errors, 0, 0);
    out->first_callback_ms = (uint64_t)_InterlockedCompareExchange64(&first_callback, 0, 0);
    out->trackpad_creates = (uint64_t)_InterlockedCompareExchange64(&trackpad_creates, 0, 0);
    out->public_create_calls = (uint64_t)_InterlockedCompareExchange64(&public_creates, 0, 0);
    out->public_update_calls = (uint64_t)_InterlockedCompareExchange64(&public_updates, 0, 0);
}

__declspec(dllexport)
int32_t mv_trial_create(void *self, uint64_t container, const char *path, uint64_t *out) {
    int32_t result = ((Create)(base() + 0xE750))(self, container, path, out);
    const uint64_t now = receipt();
    const long long target = _InterlockedCompareExchange64(&expected_create, 0, 0);
    observed(now);
    _InterlockedIncrement64(&creates);
    if (target && (long long)(*(uintptr_t **)self)[0] == target)
        _InterlockedIncrement64(&public_creates);
    if (result) _InterlockedIncrement64(&create_errors);
    if (!result && out) {
        if (trackpad(path)) _InterlockedIncrement64(&trackpad_creates);
        mv_created(&probe, container, path, *out, result);
    }
    return result;
}

__declspec(dllexport)
int32_t mv_trial_update(void *self, uint64_t component, bool value, double offset) {
    int32_t result = ((Update)(base() + 0xF0E0))(self, component, value, offset);
    const uint64_t now = receipt();
    const long long target = _InterlockedCompareExchange64(&expected_update, 0, 0);
    observed(now);
    _InterlockedIncrement64(&updates);
    if (target && (long long)(*(uintptr_t **)self)[1] == target)
        _InterlockedIncrement64(&public_updates);
    if (result) _InterlockedIncrement64(&update_errors);
    mv_updated(&probe, component, value, result, now);
    return result;
}
