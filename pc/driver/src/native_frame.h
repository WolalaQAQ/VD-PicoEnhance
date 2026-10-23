#ifndef VD_NATIVE_FRAME_H
#define VD_NATIVE_FRAME_H
#include "native_battery.h"

typedef struct {
    void *vtable;
    uint8_t operation_started, connected;
    uint8_t reserved_0a[6];
    void *mapping_handle;
    const volatile uint8_t *mapping;
    void *hands_mapping_handle, *hands_event, *hands_mapping;
    VdNativeWatcher *watcher;
    void *tracker_begin, *tracker_end, *tracker_capacity;
} VdNativeProvider;

enum { VD_BATTERY_WITHDRAWN, VD_BATTERY_PUBLISHED, VD_BATTERY_RETRY };

typedef struct {
    uint64_t container;
    int32_t percentage; /* Last successful target; -1 before a successful set. */
    uint8_t publication; /* Withdrawn, successful, or failed/possibly partial. */
} VdNativeFrameCache;

typedef void (*VdNativeOriginalFrame)(VdNativeProvider *);
typedef int32_t (*VdNativeFrameLock)(void *);
typedef void (*VdNativeFrameUnlock)(void *);
typedef struct {
    VdNativeOriginalFrame original_frame;
    VdNativeProperties *const *properties;
    const VdNativeShim *const *shims;
    void *mutex;
    VdNativeFrameLock lock;
    VdNativeFrameUnlock unlock;
    VdNativeFrameCache *cache; /* Exactly two sides, no retained shim pointers. */
} VdNativeFrameContext;

/* Called only during the original provider's initialized, pre-Cleanup lifetime. */
void native_frame_tick(const VdNativeFrameContext *ctx, VdNativeProvider *provider);
#endif
