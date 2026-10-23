#ifndef MIXEDVR_SDK_SAMPLER_H
#define MIXEDVR_SDK_SAMPLER_H
#include "input_probe.h"
#include <openvr_driver.h>

enum { MV_SDK_DEVICE_COUNT = 64, MV_SDK_SERIAL_BYTES = 256 };

/* Private identity cache: never serialize this context or log its members. */
struct MvSdkDevice {
    uint64_t container, generation;
    uint32_t role;
    bool ready;
    char serial[MV_SDK_SERIAL_BYTES];
};

/* Caller-owned, persistent storage; serial capacity includes its NUL.
 * Oversized serials leave identity incomplete, without truncation/expansion.
 * Keep this context off the RunFrame stack.
 * Init/reset only with callbacks quiesced; sample calls must be serialized. */
struct MvSdkSampler {
    MvProbe *probe;
    vr::IVRProperties *properties;
    vr::IVRServerDriverHost *host;
    uint64_t generation;
    MvSdkDevice devices[MV_SDK_DEVICE_COUNT];
    vr::TrackedDevicePose_t raw[MV_SDK_DEVICE_COUNT];
    char serial_scratch[MV_SDK_SERIAL_BYTES];
};

extern "C" {
void mv_sdk_init(MvSdkSampler *sampler, MvProbe *probe,
                 vr::IVRProperties *properties, vr::IVRServerDriverHost *host);
/* Output is indexed by tracked-device index. Return verified identity count,
 * including identities whose input component has not been created yet.
 * No serial/container/component/path is present in MvSample. */
uint32_t mv_sdk_sample(MvSdkSampler *sampler, MvSample output[MV_SDK_DEVICE_COUNT]);
}
#endif
