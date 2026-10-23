#ifndef VD_NATIVE_BATTERY_H
#define VD_NATIVE_BATTERY_H
#include <stdint.h>

/* IVRProperties_001 ABI, OpenVR SDK openvr_driver.h:3272-3316. */
typedef struct {
    int32_t property, write_type, set_error;
    void *buffer;
    uint32_t buffer_size, type_tag;
    int32_t error;
} VdNativePropertyWrite;

typedef struct VdNativeProperties VdNativeProperties;
typedef int32_t (*VdNativeWriteBatch)(VdNativeProperties *, uint64_t,
                                     VdNativePropertyWrite *, uint32_t);
typedef uint64_t (*VdNativeDeviceContainer)(VdNativeProperties *, uint32_t);
typedef struct {
    void *read_batch;
    VdNativeWriteBatch write_batch;
    void *error_name;
    VdNativeDeviceContainer device_container;
} VdNativePropertiesVtable;
struct VdNativeProperties { const VdNativePropertiesVtable *vtable; };

/* Read-only prefix of the original VD 1.34.22 controller shim. Offsets are
 * checked against native-integration-20261020.md, not a new allocated device.
 */
typedef struct { const volatile uint8_t *mapping; } VdNativeWatcher;
typedef struct {
    void *vtable, *original;
    VdNativeWatcher *watcher;
    uint8_t modes[4];
    uint32_t role;
    uint8_t reserved_20[32];
    uint32_t index;
    uint8_t reserved_44[12];
    uint8_t active;
} VdNativeShim;

/* Caller is the original VD physical-controller shim, which owns the container.
 * No device discovery, mapping creation, input handling, or background thread.
 * Scheduling and the original-component machine-code call site are not included.
 */
int32_t vd_native_publish_battery(VdNativeProperties *properties, uint64_t container,
                                   uint8_t tracked, int32_t percentage);
/* Temporary source loss: withdraw availability, retain the last known float.
 * Some overlays ignore property-read errors and mistake a missing float for 0%. */
int32_t vd_native_withdraw_battery(VdNativeProperties *properties, uint64_t container);
int32_t vd_native_update_controller(VdNativeProperties *properties, const VdNativeShim *self);
#endif
