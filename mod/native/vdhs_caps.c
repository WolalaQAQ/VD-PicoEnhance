#include <pthread.h>
#include <sys/system_properties.h>
#include <android/log.h>

#include "vdhs_caps.h"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "vdhs-layer", __VA_ARGS__)

static pthread_once_t g_hint_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t g_caps_lock = PTHREAD_MUTEX_INITIALIZER;
static vdhs_cap g_pico_etft_hint, g_eye;
static int g_eye_enabled;

static const char* cap_name(vdhs_cap cap) {
    return cap == VDHS_CAP_SUPPORTED ? "supported" : cap == VDHS_CAP_UNSUPPORTED ? "unsupported" : "unknown";
}

static void read_pico_hint(void) {
    char value[PROP_VALUE_MAX] = { 0 };
    __system_property_get("ro.pxr.eyetracking.support", value);
    g_pico_etft_hint = vdhs_caps_pico_etft_hint(value);
    LOGI("caps: PICO ET/FT hardware hint=%s (ro.pxr.eyetracking.support=%s); legacy face permission fix=%s",
         cap_name(g_pico_etft_hint), value[0] ? value : "(absent)",
         vdhs_caps_face_permission_policy(g_pico_etft_hint) ? "retained" : "disabled");
}

void vdhs_caps_begin_instance(uint32_t count, const char* const* names) {
    pthread_once(&g_hint_once, read_pico_hint);
    pthread_mutex_lock(&g_caps_lock);
    g_eye_enabled = vdhs_caps_has_eye_gaze_ext(count, names);
    __atomic_store_n(&g_eye, VDHS_CAP_UNKNOWN, __ATOMIC_RELEASE);
    LOGI("caps: instance eye-gaze ext=%d; runtime eye-gaze support pending", g_eye_enabled);
    pthread_mutex_unlock(&g_caps_lock);
}

void vdhs_caps_observe_system_properties(int32_t result, const void* next) {
    // A failed query says nothing about hardware (including permission/service
    // failures). Missing structures leave previously confirmed support intact.
    if (result != 0) return;
    pthread_mutex_lock(&g_caps_lock);
    vdhs_cap eye = __atomic_load_n(&g_eye, __ATOMIC_RELAXED);
    vdhs_cap support = vdhs_caps_update_eye_support(eye, result, g_eye_enabled, next);
    if (support != eye) {
        __atomic_store_n(&g_eye, support, __ATOMIC_RELEASE);
        LOGI("caps: eye-gaze runtime=%s (PICO ET/FT hardware hint=%s)", cap_name(support), cap_name(g_pico_etft_hint));
    }
    pthread_mutex_unlock(&g_caps_lock);
}

int vdhs_caps_eye_supported(void) {
    return __atomic_load_n(&g_eye, __ATOMIC_ACQUIRE) == VDHS_CAP_SUPPORTED;
}

int vdhs_caps_fix_face_permission(void) {
    pthread_once(&g_hint_once, read_pico_hint);
    return vdhs_caps_face_permission_policy(g_pico_etft_hint);
}

int vdhs_caps_capture_eye_settings(void) {
    pthread_once(&g_hint_once, read_pico_hint);
    return vdhs_caps_capture_eye_policy(g_pico_etft_hint, __atomic_load_n(&g_eye, __ATOMIC_ACQUIRE));
}
