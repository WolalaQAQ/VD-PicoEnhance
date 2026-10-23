/* Offline prototype for a correction inside the original VD controller shim.
 * This is not a driver or loader and is not integrated into the installed DLL.
 */
#include <stddef.h>
#include "native_battery.h"

_Static_assert(sizeof(VdNativePropertyWrite) == 40, "OpenVR x64 property-write size");
_Static_assert(offsetof(VdNativePropertyWrite, buffer) == 16, "property buffer offset");
_Static_assert(offsetof(VdNativePropertyWrite, error) == 32, "property result offset");
_Static_assert(offsetof(VdNativePropertiesVtable, write_batch) == 8, "IVRProperties slot 1");
_Static_assert(offsetof(VdNativePropertiesVtable, device_container) == 24, "IVRProperties slot 3");
_Static_assert(offsetof(VdNativeShim, original) == 8, "original device offset");
_Static_assert(offsetof(VdNativeShim, watcher) == 16, "watcher offset");
_Static_assert(offsetof(VdNativeShim, role) == 28, "role offset");
_Static_assert(offsetof(VdNativeShim, index) == 64, "tracked index offset");
_Static_assert(offsetof(VdNativeShim, active) == 80, "active offset");

int32_t vd_native_publish_battery(VdNativeProperties *properties, uint64_t container,
                                 uint8_t tracked, int32_t percentage) {
    const int available = (tracked & 3u) == 1u && percentage >= 0 && percentage <= 100;
    float level = available ? percentage / 100.0f : 0.0f;
    uint8_t provides = 1;
    VdNativePropertyWrite batch[2] = {0};
    batch[0].property = 1012;
    batch[1].property = 1026;
    if (available) {
        batch[0].buffer = &level;
        batch[0].buffer_size = sizeof level;
        batch[0].type_tag = 1;  /* k_unFloatPropertyTag */
        batch[1].buffer = &provides;
        batch[1].buffer_size = sizeof provides;
        batch[1].type_tag = 4;  /* k_unBoolPropertyTag */
    } else {
        /* The original shim owns its properties. Unknown/hand input removes its
         * battery output; it does not turn -1 into a false empty or charging value.
         */
        batch[0].write_type = batch[1].write_type = 1; /* PropertyWrite_Erase */
    }
    int32_t result = properties->vtable->write_batch(properties, container, batch, 2);
    if (result) return result;
    return batch[0].error ? batch[0].error : batch[1].error;
}

int32_t vd_native_update_controller(VdNativeProperties *properties, const VdNativeShim *self) {
    /* Excludes the independent HANDL/HANDR objects before dereferencing their
     * state. The scheduler must hold the original object's lifetime stable.
     */
    if (!self->original || !self->active || self->index == UINT32_MAX ||
        self->role < 1 || self->role > 2) return 0;
    const volatile uint8_t *mapping = self->watcher->mapping;
    const uint32_t offset = 140 + 68 * (self->role - 1);
    const uint8_t tracked = mapping[offset];
    const int32_t percentage = (int8_t)mapping[offset + 1];
    const uint64_t container = properties->vtable->device_container(properties, self->index);
    return vd_native_publish_battery(properties, container, tracked, percentage);
}

int32_t vd_native_withdraw_battery(VdNativeProperties *properties, uint64_t container) {
    VdNativePropertyWrite item = {0};
    item.property = 1026;
    item.write_type = 1; /* PropertyWrite_Erase. Do not turn unavailable into 0%. */
    const int32_t result = properties->vtable->write_batch(properties, container, &item, 1);
    return result ? result : item.error;
}
