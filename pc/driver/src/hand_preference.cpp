#include "hand_preference.h"
#include <stddef.h>

static bool equal(const char *a, const char *b) {
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}
static bool typed(const vr::PropertyRead_t &r, vr::PropertyTypeTag_t tag) {
    return r.eError == vr::TrackedProp_Success && r.unTag == tag &&
        r.unRequiredBufferSize && r.unRequiredBufferSize <= r.unBufferSize;
}
static bool text(const vr::PropertyRead_t &r) {
    if (!typed(r, vr::k_unStringPropertyTag)) return false;
    for (uint32_t i = 0; i < r.unRequiredBufferSize; ++i)
        if (!static_cast<const char *>(r.pvBuffer)[i]) return i + 1 == r.unRequiredBufferSize;
    return false;
}
static uint32_t hand(vr::IVRProperties *p, uint64_t container) {
    int32_t cls = 0, role = 0;
    char driver[32] = {}, kind[32] = {}, serial[16] = {};
    vr::PropertyRead_t r[] = {
        {vr::Prop_DeviceClass_Int32, &cls, sizeof(cls), 0, 0, vr::TrackedProp_UnknownProperty},
        {vr::Prop_ActualTrackingSystemName_String, driver, sizeof(driver), 0, 0, vr::TrackedProp_UnknownProperty},
        {vr::Prop_ControllerType_String, kind, sizeof(kind), 0, 0, vr::TrackedProp_UnknownProperty},
        {vr::Prop_ControllerRoleHint_Int32, &role, sizeof(role), 0, 0, vr::TrackedProp_UnknownProperty},
        {vr::Prop_SerialNumber_String, serial, sizeof(serial), 0, 0, vr::TrackedProp_UnknownProperty}
    };
    if (p->ReadPropertyBatch(container, r, 5) != vr::TrackedProp_Success ||
        !typed(r[0], vr::k_unInt32PropertyTag) || r[0].unRequiredBufferSize != sizeof(cls) ||
        !typed(r[3], vr::k_unInt32PropertyTag) || r[3].unRequiredBufferSize != sizeof(role) ||
        !text(r[1]) || !text(r[2]) || !text(r[4]) || cls != vr::TrackedDeviceClass_Controller ||
        !equal(driver, "VirtualDesktop") || !equal(kind, "vd_hand_controller")) return 0;
    if (role == vr::TrackedControllerRole_LeftHand && equal(serial, "HANDL")) return 1;
    if (role == vr::TrackedControllerRole_RightHand && equal(serial, "HANDR")) return 2;
    return 0;
}
static vr::ETrackedPropertyError priority(vr::IVRProperties *p, uint64_t container,
                                         int32_t &value, vr::PropertyTypeTag_t &tag) {
    vr::PropertyRead_t r = {vr::Prop_ControllerHandSelectionPriority_Int32, &value, sizeof(value),
                            0, 0, vr::TrackedProp_UnknownProperty};
    const auto batch = p->ReadPropertyBatch(container, &r, 1);
    tag = r.unTag;
    if (batch != vr::TrackedProp_Success) return batch;
    if (r.eError != vr::TrackedProp_Success) return r.eError;
    return typed(r, vr::k_unInt32PropertyTag) && r.unRequiredBufferSize == sizeof(value)
        ? vr::TrackedProp_Success : vr::TrackedProp_WrongDataType;
}
static vr::ETrackedPropertyError write(vr::IVRProperties *p, uint64_t container, int32_t value) {
    vr::PropertyWrite_t item = {};
    item.prop = vr::Prop_ControllerHandSelectionPriority_Int32;
    item.writeType = vr::PropertyWrite_Set;
    item.pvBuffer = &value;
    item.unBufferSize = sizeof(value);
    item.unTag = vr::k_unInt32PropertyTag;
    const auto batch = p->WritePropertyBatch(container, &item, 1);
    return batch == vr::TrackedProp_Success ? item.eError : batch;
}
static void emit(MvHandPreference *s, const char *status, uint32_t role,
                 int32_t baseline, vr::ETrackedPropertyError error, uint32_t matched = 0) {
    const MvHandPreferenceEvent e = {status, role, matched, baseline, error};
    if (s->sink) s->sink(s->user, &e);
}
void mv_hand_preference_init(MvHandPreference *s, vr::IVRProperties *p, bool enabled,
                             MvHandPreferenceSink sink, void *user) {
    auto *bytes = reinterpret_cast<unsigned char *>(s);
    for (size_t i = 0; i < sizeof(*s); ++i) bytes[i] = 0;
    s->properties = p;
    s->enabled = enabled;
    s->sink = sink;
    s->user = user;
}
void mv_hand_preference_apply_pending(MvHandPreference *s) {
    if (!s->enabled) return;
    uint64_t containers[2] = {};
    uint32_t indices[2] = {}, counts[2] = {};
    for (uint32_t index = 0; index < vr::k_unMaxTrackedDeviceCount; ++index) {
        const auto container = s->properties->TrackedDeviceToPropertyContainer(index);
        const auto role = container ? hand(s->properties, container) : 0;
        if (!role) continue;
        containers[role - 1] = container;
        indices[role - 1] = index;
        ++counts[role - 1];
    }
    for (uint32_t side = 0; side < 2; ++side) {
        if (!counts[side]) continue;
        auto &e = s->hands[side];
        const uint32_t role = side + 1;
        if (counts[side] != 1) { emit(s, "ambiguous", role, 0, vr::TrackedProp_InvalidOperation); continue; }
        if (e.captured && (e.container != containers[side] || e.index != indices[side])) {
            emit(s, "identity_changed", role, e.baseline, vr::TrackedProp_InvalidDevice); continue;
        }
        if (e.captured) continue;
        e.baseline_error = priority(s->properties, containers[side], e.baseline, e.baseline_tag);
        if (e.baseline_error != vr::TrackedProp_Success) {
            emit(s, "baseline_unhandled", role, 0, e.baseline_error); continue;
        }
        e.container = containers[side];
        e.index = indices[side];
        e.captured = true;
        if (e.baseline == 100) { emit(s, "already_100", role, e.baseline, vr::TrackedProp_Success); continue; }
        e.attempted = true;
        auto error = write(s->properties, e.container, 100);
        if (error != vr::TrackedProp_Success) { emit(s, "apply_failed", role, e.baseline, error); continue; }
        int32_t current = 0;
        vr::PropertyTypeTag_t tag = 0;
        error = priority(s->properties, e.container, current, tag);
        if (error != vr::TrackedProp_Success || current != 100) {
            emit(s, "apply_unconfirmed", role, e.baseline,
                 error == vr::TrackedProp_Success ? vr::TrackedProp_InvalidOperation : error); continue;
        }
        e.applied = true;
        emit(s, "applied", role, e.baseline, vr::TrackedProp_Success);
    }
    emit(s, "summary", 0, 0, vr::TrackedProp_Success, counts[0] + counts[1]);
}
void mv_hand_preference_restore(MvHandPreference *s) {
    s->enabled = false;
    for (uint32_t side = 0; side < 2; ++side) {
        auto &e = s->hands[side];
        const uint32_t role = side + 1;
        if (!e.attempted || e.finished) continue;
        if (s->properties->TrackedDeviceToPropertyContainer(e.index) != e.container ||
            hand(s->properties, e.container) != role) {
            emit(s, "restore_identity_lost", role, e.baseline, vr::TrackedProp_InvalidDevice); continue;
        }
        int32_t current = 0;
        vr::PropertyTypeTag_t tag = 0;
        auto error = priority(s->properties, e.container, current, tag);
        if (error != vr::TrackedProp_Success) { emit(s, "restore_read_failed", role, e.baseline, error); continue; }
        if (current == e.baseline) {
            e.finished = true;
            emit(s, "already_restored", role, e.baseline, vr::TrackedProp_Success); continue;
        }
        if (current != 100) { emit(s, "restore_conflict", role, e.baseline, vr::TrackedProp_InvalidOperation); continue; }
        error = write(s->properties, e.container, e.baseline);
        if (error == vr::TrackedProp_Success) error = priority(s->properties, e.container, current, tag);
        if (error != vr::TrackedProp_Success || current != e.baseline) {
            emit(s, "restore_failed", role, e.baseline,
                 error == vr::TrackedProp_Success ? vr::TrackedProp_InvalidOperation : error); continue;
        }
        e.finished = true;
        emit(s, "restored", role, e.baseline, vr::TrackedProp_Success);
    }
}
