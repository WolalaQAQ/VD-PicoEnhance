// PICO capability policy. Keep the firmware's ET/FT hardware hint separate from
// runtime eye-gaze support. Activity and Android grants prove neither capability.
#pragma once
#include <stdint.h>
#include <string.h>

typedef enum {
    VDHS_CAP_UNKNOWN = 0,
    VDHS_CAP_UNSUPPORTED = 1,
    VDHS_CAP_SUPPORTED = 2,
} vdhs_cap;

// Khronos OpenXR / VD's Xenko.OpenXR.StructureType. In particular, 1000030001
// is EYE_GAZE_SAMPLE_TIME_EXT, not SYSTEM_EYE_GAZE_INTERACTION_PROPERTIES_EXT.
enum { VDHS_CAP_TYPE_EYE_GAZE = 1000030000u };
// These views alias the application's OpenXR structs, not objects declared with
// our local typedefs. Preserve that aliasing under the payload's -O2 build.
typedef struct __attribute__((__may_alias__)) { uint32_t type; const void* next; } vdhs_caps_base;
typedef struct __attribute__((__may_alias__)) { uint32_t type; const void* next; uint32_t supported; } vdhs_caps_property;

// PICO-documentation/settings describes ro.pxr.eyetracking.support as an ET/FT
// hardware flag. It is not a generic implication from eye gaze to face support.
// https://github.com/thoricelli/PICO-documentation/blob/main/settings/README.md
static inline vdhs_cap vdhs_caps_pico_etft_hint(const char* value) {
    if (!strcmp(value, "1")) return VDHS_CAP_SUPPORTED;
    if (!strcmp(value, "0")) return VDHS_CAP_UNSUPPORTED;
    return VDHS_CAP_UNKNOWN;
}

static inline int vdhs_caps_has_eye_gaze_ext(uint32_t count, const char* const* names) {
    for (uint32_t i = 0; i < count; i++) {
        if (!names[i]) continue;
        if (!strcmp(names[i], "XR_EXT_eye_gaze_interaction")) return 1;
    }
    return 0;
}

// VD's PICO whitelist enables EXT eye gaze, not FB/ANDROID face tracking. Its
// unused, zero-initialized face property structs are not a PICO hardware test.
static inline vdhs_cap vdhs_caps_read_eye_support(int enabled, const void* next) {
    if (!enabled) return VDHS_CAP_UNKNOWN;
    for (const vdhs_caps_base* p = next; p; p = p->next) {
        if (p->type == VDHS_CAP_TYPE_EYE_GAZE)
            return ((const vdhs_caps_property*)p)->supported ? VDHS_CAP_SUPPORTED : VDHS_CAP_UNSUPPORTED;
    }
    return VDHS_CAP_UNKNOWN;
}

static inline vdhs_cap vdhs_caps_update_eye_support(vdhs_cap previous, int32_t result,
                                                   int enabled, const void* next) {
    if (result != 0) return previous;
    vdhs_cap support = vdhs_caps_read_eye_support(enabled, next);
    return support == VDHS_CAP_UNKNOWN ? previous : support;
}

static inline int vdhs_caps_capture_eye_policy(vdhs_cap hint, vdhs_cap runtime) {
    // Capturing a saved value is read-only. Unknown may be captured early, but
    // it must not enable an eye permission rewrite, restore or gaze enhancement.
    return runtime == VDHS_CAP_UNKNOWN ? hint != VDHS_CAP_UNSUPPORTED : runtime == VDHS_CAP_SUPPORTED;
}

static inline int vdhs_caps_face_permission_policy(vdhs_cap pico_etft_hint) {
    // Preserve VD's existing PICO permission-name workaround unless the firmware
    // explicitly says ET/FT hardware is absent. Missing property stays UNKNOWN,
    // not supported. This does not enable/advertise a face tracker or grant an
    // Android permission, and does not use eye-gaze support as face evidence.
    return pico_etft_hint != VDHS_CAP_UNSUPPORTED;
}

void vdhs_caps_begin_instance(uint32_t count, const char* const* names);
void vdhs_caps_observe_system_properties(int32_t result, const void* next);
int vdhs_caps_eye_supported(void);
int vdhs_caps_fix_face_permission(void);
int vdhs_caps_capture_eye_settings(void);
