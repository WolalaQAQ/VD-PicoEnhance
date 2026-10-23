extern "C" {
#include "input_probe.h"
}
#include "sdk_sampler.h"
#include <stddef.h>

static_assert(MV_SDK_DEVICE_COUNT == vr::k_unMaxTrackedDeviceCount, "SDK device count");
static_assert(MV_SDK_SERIAL_BYTES <= vr::k_unMaxPropertyStringSize, "SDK string limit");

static bool equal(const char *a, const char *b) {
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}

static bool property_ok(const vr::PropertyRead_t &read, vr::PropertyTypeTag_t tag) {
    return read.eError == vr::TrackedProp_Success && read.unTag == tag &&
        read.unRequiredBufferSize > 0 && read.unRequiredBufferSize <= read.unBufferSize;
}

static bool string_ok(const vr::PropertyRead_t &read) {
    if (!property_ok(read, vr::k_unStringPropertyTag)) return false;
    const char *text = static_cast<const char *>(read.pvBuffer);
    /* Reject embedded NUL/truncated strings instead of accepting a prefix. */
    for (uint32_t i = 0; i < read.unRequiredBufferSize; ++i)
        if (!text[i]) return i + 1 == read.unRequiredBufferSize;
    return false;
}

void mv_sdk_init(MvSdkSampler *s, MvProbe *probe, vr::IVRProperties *properties,
                 vr::IVRServerDriverHost *host) {
    auto *bytes = reinterpret_cast<unsigned char *>(s);
    for (size_t i = 0; i < sizeof(*s); ++i) bytes[i] = 0;
    s->probe = probe;
    s->properties = properties;
    s->host = host;
}

static MvIdentity identity(const MvSdkDevice &d, uint32_t index) {
    return {d.container, d.generation, index, d.role, 1};
}

uint32_t mv_sdk_sample(MvSdkSampler *s, MvSample output[MV_SDK_DEVICE_COUNT]) {
    s->host->GetRawTrackedDevicePoses(0.0f, s->raw, MV_SDK_DEVICE_COUNT);
    uint32_t matched = 0;
    for (uint32_t index = 0; index < MV_SDK_DEVICE_COUNT; ++index) {
        MvSdkDevice &d = s->devices[index];
        MvSample &out = output[index];
        out = {};
        const auto container = s->properties->TrackedDeviceToPropertyContainer(index);
        int32_t device_class = 0, role = 0;
        char actual_tracking[sizeof("lighthouse")] = {}, type[sizeof("knuckles")] = {};
        char *serial = s->serial_scratch;
        vr::PropertyRead_t batch[] = {
            {vr::Prop_DeviceClass_Int32, &device_class, sizeof(device_class), 0, 0, vr::TrackedProp_UnknownProperty},
            {vr::Prop_ActualTrackingSystemName_String, actual_tracking, sizeof(actual_tracking), 0, 0, vr::TrackedProp_UnknownProperty},
            {vr::Prop_ControllerType_String, type, sizeof(type), 0, 0, vr::TrackedProp_UnknownProperty},
            {vr::Prop_ControllerRoleHint_Int32, &role, sizeof(role), 0, 0, vr::TrackedProp_UnknownProperty},
            {vr::Prop_SerialNumber_String, serial, MV_SDK_SERIAL_BYTES, 0, 0, vr::TrackedProp_UnknownProperty}
        };
        bool ready = container && s->properties->ReadPropertyBatch(container, batch, 5) == vr::TrackedProp_Success;
        ready = ready && property_ok(batch[0], vr::k_unInt32PropertyTag) &&
            batch[0].unRequiredBufferSize == sizeof(device_class) &&
            property_ok(batch[3], vr::k_unInt32PropertyTag) &&
            batch[3].unRequiredBufferSize == sizeof(role) &&
            string_ok(batch[1]) && string_ok(batch[2]) && string_ok(batch[4]) &&
            device_class == vr::TrackedDeviceClass_Controller && equal(actual_tracking, "lighthouse") &&
            equal(type, "knuckles") && serial[0] &&
            (role == vr::TrackedControllerRole_LeftHand || role == vr::TrackedControllerRole_RightHand);
        if (!ready) {
            if (d.ready) {
                /* Revoke an already-bound identity; do not discard pending
                 * pre-identity creation/update events on its first discovery. */
                const MvIdentity old = identity(d, index);
                const MvPose disconnected = {};
                MvSample ignored;
                mv_sample(s->probe, &old, &disconnected, &ignored);
                d.ready = false;
            }
        } else {
            if (!d.ready || d.container != container || d.role != static_cast<uint32_t>(role) ||
                !equal(d.serial, serial)) {
                d.container = container;
                d.role = static_cast<uint32_t>(role);
                d.generation = ++s->generation;
                for (uint32_t i = 0; i < batch[4].unRequiredBufferSize; ++i) d.serial[i] = serial[i];
                d.ready = true;
            }
            const MvIdentity id = identity(d, index);
            MvPose pose = {};
            pose.connected = s->raw[index].bDeviceIsConnected;
            pose.valid = s->raw[index].bPoseIsValid;
            pose.tracking_result = s->raw[index].eTrackingResult;
            for (uint32_t axis = 0; axis < 3; ++axis) {
                pose.velocity[axis] = s->raw[index].vVelocity.v[axis];
                pose.angular_velocity[axis] = s->raw[index].vAngularVelocity.v[axis];
            }
            mv_sample(s->probe, &id, &pose, &out);
            out.role = d.role;
            ++matched;
        }
        out.device_index = index;
    }
    return matched;
}
