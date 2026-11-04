#include <stddef.h>
#include "native_frame.h"

#ifndef VD_ENTRY_RVA
#error VD_ENTRY_RVA must match the final vd_native_run_frame export RVA
#endif

int _fltused;
void *memset(void *destination, int value, size_t count) {
    for (size_t i = 0; i < count; ++i) ((uint8_t *)destination)[i] = (uint8_t)value;
    return destination;
}

static VdNativeFrameCache cache[2] = {{0, -1, 0}, {0, -1, 0}};

/* Filled by the local installer after matching the exact official driver. */
__declspec(dllexport) const volatile uint32_t vd_native_image_layout[3] = {
    0x12200, 0x19D00, 0x19D48
};

__declspec(dllexport) __attribute__((section(".text$a")))
void vd_native_run_frame(VdNativeProvider *self) {
    const uintptr_t base = (uintptr_t)&vd_native_run_frame - (uintptr_t)VD_ENTRY_RVA;
    const VdNativeFrameContext ctx = {
        (VdNativeOriginalFrame)(base + vd_native_image_layout[0]),
        (VdNativeProperties *const *)(base + 0x9A6C0),
        (const VdNativeShim *const *)(base + 0x9A740),
        (void *)(base + 0x99250),
        (VdNativeFrameLock)(base + vd_native_image_layout[1]),
        (VdNativeFrameUnlock)(base + vd_native_image_layout[2]),
        cache
    };
    native_frame_tick(&ctx, self);
}
