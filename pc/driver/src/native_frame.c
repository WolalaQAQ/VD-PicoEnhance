#include <stddef.h>
#include "native_frame.h"

_Static_assert(offsetof(VdNativeProvider, connected) == 9, "provider connected offset");
_Static_assert(offsetof(VdNativeProvider, mapping) == 24, "provider main mapping offset");
_Static_assert(offsetof(VdNativeProvider, watcher) == 56, "provider watcher offset");
_Static_assert(sizeof(VdNativeProvider) == 88, "original provider size");
_Static_assert(sizeof(VdNativeFrameCache) == 16, "per-side cache size");
_Static_assert(offsetof(VdNativeFrameCache, publication) == 12, "publication uses former padding");

static int clear_output(VdNativeProperties *properties, VdNativeFrameCache *cache) {
    if (!cache->container) return 1;
    cache->publication = VD_BATTERY_RETRY;
    if (vd_native_publish_battery(properties, cache->container, 0, -1)) return 0;
    cache->container = 0;
    cache->percentage = -1;
    cache->publication = VD_BATTERY_WITHDRAWN;
    return 1;
}

static void withdraw_output(VdNativeProperties *properties, VdNativeFrameCache *cache) {
    if (cache->container && cache->publication != VD_BATTERY_WITHDRAWN) {
        cache->publication = VD_BATTERY_RETRY;
        if (!vd_native_withdraw_battery(properties, cache->container))
            cache->publication = VD_BATTERY_WITHDRAWN;
    }
}

void native_frame_tick(const VdNativeFrameContext *ctx, VdNativeProvider *provider) {
    ctx->original_frame(provider);
    if (!provider->connected || !provider->mapping || !provider->watcher) return;
    if (ctx->lock(ctx->mutex)) return;

    VdNativeProperties *properties = *ctx->properties;
    for (uint32_t side = 0; side < 2; ++side) {
        VdNativeFrameCache *cache = &ctx->cache[side];
        const VdNativeShim *shim = ctx->shims[side];
        if (!shim || !shim->original || !shim->active || shim->index == UINT32_MAX ||
            shim->role != side + 1 || shim->watcher != provider->watcher) {
            clear_output(properties, cache);
            continue;
        }
        const uint32_t offset = 140 + 68 * side;
        const uint8_t tracked = provider->mapping[offset];
        const int32_t percentage = (int8_t)provider->mapping[offset + 1];
        const uint64_t container = properties->vtable->device_container(properties, shim->index);
        if (!container) {
            clear_output(properties, cache);
            continue;
        }
        if (cache->container != container) {
            if (!clear_output(properties, cache)) continue;
            /* Retain an attempted destination too: a failed batch can have written
             * one property, so source loss must still be able to erase our output. */
            cache->container = container;
        }
        if ((tracked & 3u) != 1u || percentage < 0 || percentage > 100) {
            withdraw_output(properties, cache);
            continue;
        }
        if (cache->publication != VD_BATTERY_PUBLISHED || cache->percentage != percentage) {
            /* Failed batches remain retryable even when the old value returns. */
            cache->publication = VD_BATTERY_RETRY;
            if (!vd_native_publish_battery(properties, container, tracked, percentage)) {
                cache->percentage = percentage;
                cache->publication = VD_BATTERY_PUBLISHED;
            }
        }
    }
    ctx->unlock(ctx->mutex);
}
