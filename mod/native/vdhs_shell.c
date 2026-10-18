// Optional Shell-side gesture suppression, adapted from misaka18848's PR #1.
// This payload is only staged when explicitly enabled in full module mode.
// It does not run in the OpenXR runtime service or change system properties.
#define _GNU_SOURCE
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <sys/system_properties.h>
#include <unistd.h>
#include <android/log.h>

#include "shadowhook.h"
#include "vdhs_sym.h"
#include "vdhs_shell_policy.h"
#include "vdhs_shell_abi.h"

#define TAG "vdhs-shell"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

static const char* kLibrary = "libxrshell.so";
static const char* kUpdate =
    "_ZN12InputManager30updateAppDisableSystemGesturesERKNSt6__ndk112basic_stringIcNS0_11char_traitsIcEENS0_9allocatorIcEEEERK20XrServerDisplayStateb";
static const char* kFlag = "gApplicationDisableSystemGestures";
static const char* kList = "gDisableSystemGesturesPackageNames";
static const char* kVd = "VirtualDesktop.Android";
static const char* kKnown = "com.picovr.picostreamassistant";
// Mirrored from VD's Input-tab switch by the module's service.sh.
static const char* kSwitchProperty = "vdhs.system_gestures";

typedef void (*PFN_update)(void*, const char*, const void*, int);
static PFN_update g_original;
static unsigned char* g_disabled;

static void hook_update(void* self, const char* package, const void* state, int is_self) {
    if (package && vdhs_shell_string_is(package, kVd)) {
        // The original rechecks its whitelist every frame. Only override VD's
        // decision; all other packages retain the complete original path.
        unsigned char disabled = state ? (unsigned char)vdhs_shell_disable_for_vd(*(const int*)state, is_self) : 0;

        // Live switch: the module mirrors VD's own Input-tab checkbox into this
        // property, so the user can turn suppression off without a reboot. An
        // empty or missing value keeps the decision above.
        char flag[PROP_VALUE_MAX] = {0};
        if (__system_property_get(kSwitchProperty, flag) > 0) {
            if (flag[0] == '0') disabled = 0;
            else if (flag[0] == '1') disabled = 1;
        }

        if (*g_disabled != disabled) {
            *g_disabled = disabled;
            LOGI("system gestures %s for VD", disabled ? "suppressed" : "restored");
        }
        return;
    }
    g_original(self, package, state, is_self);
}

static void* shell_thread(void* unused) {
    (void)unused;
    const char* names[] = { kUpdate, kFlag, kList };
    void* symbols[3] = { NULL, NULL, NULL };
    int ready = 0;
    // Wait for the library and its C++ static initializers, not just mappings.
    for (int i = 0; i < 600 && !ready; ++i) {
        if (vdhs_resolve_mapped_symbols(kLibrary, names, symbols, 3) == 3)
            ready = vdhs_shell_known_list_layout(symbols[2], kKnown);
        if (!ready) usleep(100000);
    }
    if (!ready) { LOGE("Shell symbols/layout unavailable; gesture suppression not installed"); return NULL; }
    int rc = shadowhook_init(SHADOWHOOK_MODE_UNIQUE, false);
    if (rc != 0) { LOGE("ShadowHook init failed: %s", shadowhook_to_errmsg(rc)); return NULL; }
    g_disabled = (unsigned char*)symbols[1];
    void* stub = shadowhook_hook_func_addr(symbols[0], (void*)hook_update, (void**)&g_original);
    if (!stub) LOGE("Shell hook failed: %s", shadowhook_to_errmsg(shadowhook_get_errno()));
    else LOGI("optional Shell gesture suppression installed");
    return NULL;
}

__attribute__((visibility("default")))
int vdhs_shell_main(void* java_vm, int mode) {
    (void)java_vm;
    if (mode != 2) return 0;
    pthread_t thread;
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
    int rc = pthread_create(&thread, &attributes, shell_thread, NULL);
    pthread_attr_destroy(&attributes);
    return rc;
}
