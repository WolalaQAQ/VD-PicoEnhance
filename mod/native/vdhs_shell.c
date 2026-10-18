// Payload for the system shell process (com.picoxr.xrshell).
//
// The shell is what actually performs the system hand gestures (recenter / open
// dock). Its InputManager keeps a list of packages for which it refuses to act:
//
//     std::vector<std::string> gDisableSystemGesturesPackageNames
//     bool                     gApplicationDisableSystemGestures
//     InputManager::updateAppDisableSystemGestures(pkg, displayState, isSelf)
//
// updateAppDisableSystemGestures() compares the focused package against that
// list and sets the flag. The list is built in a static initialiser and contains
// exactly one hardcoded entry, "com.picovr.picostreamassistant" — that is how
// PICO's own streaming app keeps gestures from interrupting a stream.
//
// This payload hooks that function and treats VirtualDesktop.Android the same
// way, so the shell stops acting on gestures while VD is the focused app and
// behaves normally again as soon as it is not. No other process is touched.
#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <android/log.h>

#include "shadowhook.h"
#include "vdhs_sym.h"

#define TAG "vdhs-shell"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

static const char* kShellLib = "libxrshell.so";

// InputManager::updateAppDisableSystemGestures(std::string const&,
//                                              XrServerDisplayState const&, bool)
static const char* kSymUpdate =
    "_ZN12InputManager30updateAppDisableSystemGesturesERKNSt6__ndk112basic_stringIcNS0_11char_traitsIcEENS0_9allocatorIcEEEERK20XrServerDisplayStateb";
static const char* kSymFlag = "gApplicationDisableSystemGestures";
static const char* kSymList = "gDisableSystemGesturesPackageNames";

static const char* kVd = "VirtualDesktop.Android";
// The package the shell already knows about, used to sanity check the layout.
static const char* kKnown = "com.picovr.picostreamassistant";

typedef void (*PFN_update)(void*, const char*, const void*, int);
static PFN_update g_orig_update;
static unsigned char* g_flag;

// libc++ std::string layout: bit 0 of the first byte marks the long form.
static const char* cxx_data(const char* s) {
    return ((unsigned char)s[0] & 1) ? *(const char* const*)(s + 0x10) : s + 1;
}
static size_t cxx_size(const char* s) {
    unsigned char b = (unsigned char)s[0];
    return (b & 1) ? *(const size_t*)(s + 8) : (size_t)(b >> 1);
}
static int str_is(const char* s, const char* want) {
    size_t n = cxx_size(s);
    return n == strlen(want) && memcmp(cxx_data(s), want, n) == 0;
}

static void my_update(void* self, const char* pkg, const void* state, int is_self) {
    if (g_flag && pkg && str_is(pkg, kVd)) {
        // The shell re-evaluates this every frame. Answer for VD here and skip
        // the original, which would otherwise clear the flag again (it only
        // knows its own list) and log a change every frame.
        int want = (state && *(const int*)state == 6 && !is_self) ? 1 : 0;
        if (*g_flag != (unsigned char)want) {
            *g_flag = (unsigned char)want;
            LOGI("system gestures %s (VD focused)", want ? "disabled" : "enabled");
        }
        return;
    }
    g_orig_update(self, pkg, state, is_self);
}

// Read back the shell's list to confirm the layout we hook against.
static void dump_list(void* vec) {
    const char* begin = *(const char* const*)vec;
    const char* end = *(const char* const*)((const char*)vec + 8);
    int n = (int)((end - begin) / 0x18);
    LOGI("shell list has %d entr%s", n, n == 1 ? "y" : "ies");
    for (const char* p = begin; p < end; p += 0x18) {
        LOGI("  entry: %.*s", (int)cxx_size(p), cxx_data(p));
        if (str_is(p, kKnown)) LOGI("  (matches the hardcoded streaming app)");
    }
}

static void* shell_thread(void* arg) {
    (void)arg;
    int rc = shadowhook_init(SHADOWHOOK_MODE_UNIQUE, false);
    LOGI("shadowhook %s init=%d", shadowhook_get_version(), rc);
    if (rc != 0) { LOGE("shadowhook init failed: %s", shadowhook_to_errmsg(rc)); return NULL; }

    const char* names[3] = { kSymUpdate, kSymFlag, kSymList };
    void* found[3] = { NULL, NULL, NULL };
    for (int i = 0; i < 600 && !found[0]; i++) {          // ~60 s
        vdhs_resolve_mapped_symbols(kShellLib, names, found, 3);
        if (!found[0]) usleep(100000);
    }
    if (!found[0]) { LOGE("updateAppDisableSystemGestures not found"); return NULL; }
    LOGI("resolved update=%p flag=%p list=%p", found[0], found[1], found[2]);
    if (found[1]) g_flag = (unsigned char*)found[1];
    if (found[2]) dump_list(found[2]);

    void* stub = shadowhook_hook_func_addr(found[0], (void*)my_update, (void**)&g_orig_update);
    LOGI("hook updateAppDisableSystemGestures %s (%s)", stub ? "installed" : "FAILED",
         stub ? "ok" : shadowhook_to_errmsg(shadowhook_get_errno()));
    return NULL;
}

static void spawn(void* (*fn)(void*), const char* what) {
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    int rc = pthread_create(&t, &a, fn, NULL);
    pthread_attr_destroy(&a);
    if (rc != 0) LOGE("%s thread failed: %d", what, rc);
}

__attribute__((visibility("default")))
int vdhs_shell_main(void* javaVM, int mode) {
    (void)javaVM;
    LOGI("shell payload start (mode=%d)", mode);
    spawn(shell_thread, "shell");
    return 0;
}
