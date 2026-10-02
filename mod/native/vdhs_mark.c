// Canonical source: VD-PicoEnhance/mod/native/vdhs_mark.c
//
// Mirrors what PICO's libpico_openxr_loader.so does after loading the runtime:
//   runtime_gipa(XR_NULL_HANDLE, "xrMarkApiClass", &fn); fn(mask);
// The Khronos loader bundled with VD never makes this call, so the runtime keeps the PICO
// extensions out of its advertised list. We reach the runtime through the same system
// forward loader named in /vendor/etc/openxr/1/active_runtime.json.
//
// This file is the SINGLE SOURCE shared by both tracks (design §4/§6):
//   Track R: patch/native/vdhs_mark.c #includes this file, the patched
//            Xenko.OpenXR.dll P/Invokes vdhs_mark().
//   Track Z: the Zygisk payload is this same libvdhs.so; VdHsMod uses
//            vdhs_mark_loaded() / vdhs_active_input().
//
// Exports:
//   int  vdhs_mark(const void* loaderInitInfo, uint64_t mask)  (v9, unchanged)
//   int  vdhs_mark_loaded(void)                                (Track Z)
//   int  vdhs_active_input(uint64_t instance)                  (Track Z)
#include <dlfcn.h>
#include <jni.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <android/log.h>

#define TAG "vd-handswitch"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)

typedef int32_t XrResult;
typedef uint64_t XrVersion;
typedef uint64_t XrInstance;
typedef void (*PFN_xrVoidFunction)(void);
typedef XrResult (*PFN_xrGetInstanceProcAddr)(XrInstance, const char*, PFN_xrVoidFunction*);
typedef XrResult (*PFN_xrInitializeLoaderKHR)(const void*);
typedef XrResult (*PFN_xrMarkApiClass)(uint64_t);

typedef struct {
    uint32_t structType, structVersion; size_t structSize;
    uint32_t minInterfaceVersion, maxInterfaceVersion;
    XrVersion minApiVersion, maxApiVersion;
} XrNegotiateLoaderInfo;

typedef struct {
    uint32_t structType, structVersion; size_t structSize;
    uint32_t runtimeInterfaceVersion; XrVersion runtimeApiVersion;
    PFN_xrGetInstanceProcAddr getInstanceProcAddr;
} XrNegotiateRuntimeRequest;

typedef XrResult (*PFN_xrNegotiateLoaderRuntimeInterface)(const XrNegotiateLoaderInfo*, XrNegotiateRuntimeRequest*);

typedef struct { uint32_t type; const void* next; char extensionName[128]; uint32_t extensionVersion; } XrExtensionProperties;
typedef XrResult (*PFN_xrEnumerateInstanceExtensionProperties)(const char*, uint32_t, uint32_t*, XrExtensionProperties*);

#define XR_MAKE_VERSION(ma, mi, pa) ((((uint64_t)(ma)) << 48) | (((uint64_t)(mi) & 0xffff) << 32) | ((pa) & 0xffffffffULL))

// The mark mask v9 ships: enables the controller-function + hand-tracking
// extension state bits (E-025).
#define VDHS_MARK_MASK 0x3bULL

static PFN_xrGetInstanceProcAddr g_gipa = 0;   // runtime GIPA, cached after first negotiate

// Open the system forward loader and negotiate a runtime interface. Idempotent.
// Returns 0 on success, negative step on failure.
// Provided by vdhs_payload.c (Track Z only). Weak so Track R's standalone
// build of this file still links; there it is NULL and we use dlopen.
extern void* vdhs_resolve_mapped_symbol(const char* lib, const char* name) __attribute__((weak));

static int ensure_runtime(void) {
    if (g_gipa) return 0;
    PFN_xrNegotiateLoaderRuntimeInterface neg = 0;
    if (vdhs_resolve_mapped_symbol) {
        // Track Z: this library was dlopened by the Zygisk module in the
        // default linker namespace. dlopen("libopenxr_forwardloader.so") from
        // here yields a SECOND, uninitialised loader instance and calling it
        // crashes VD (SIGSEGV, observed). Bind to the copy VD already mapped
        // in its classloader namespace, via its in-memory .dynsym.
        neg = (PFN_xrNegotiateLoaderRuntimeInterface)
            vdhs_resolve_mapped_symbol("libopenxr_forwardloader.so", "xrNegotiateLoaderRuntimeInterface");
        if (!neg) return -1;   // not mapped yet; caller may retry
        // Negotiate dereferences the forward loader's exported global `vm`
        // (JavaVM*, set once VD calls its JNI_OnLoad / xrInitializeLoaderKHR)
        // without a NULL check: calling it earlier SIGSEGVs at +0x44 (observed,
        // disassembly of /system/lib64/libopenxr_forwardloader.so). Gate on it.
        void* const* vm = (void* const*)
            vdhs_resolve_mapped_symbol("libopenxr_forwardloader.so", "vm");
        if (!vm || !*vm) return -1;    // loader not initialised yet; retry
    } else {
        void* h = dlopen("libopenxr_forwardloader.so", RTLD_NOW);
        if (!h) { LOGI("mark: dlopen failed: %s", dlerror()); return -1; }
        neg = (PFN_xrNegotiateLoaderRuntimeInterface)dlsym(h, "xrNegotiateLoaderRuntimeInterface");
        if (!neg) { LOGI("mark: dlsym negotiate failed"); return -2; }
    }

    XrNegotiateLoaderInfo li = { 1, 1, sizeof li, 1, 1, XR_MAKE_VERSION(1, 0, 0), XR_MAKE_VERSION(1, 0x3ff, 0xfff) };
    XrNegotiateRuntimeRequest rr = { 3, 1, sizeof rr, 0, 0, 0 };
    XrResult r = neg(&li, &rr);
    static XrResult last_r = 1;
    if (r != last_r) {   // log transitions only; the deferred waiter polls this
        LOGI("mark: negotiate=%d gipa=%p iface=%u", r, rr.getInstanceProcAddr, rr.runtimeInterfaceVersion);
        last_r = r;
    }
    // -6 (XR_ERROR_INITIALIZATION_FAILED) is returned until VD has run
    // xrInitializeLoaderKHR on the forward loader (observed): retryable.
    if (r == -6 && vdhs_resolve_mapped_symbol) return -1;
    if (r != 0 || !rr.getInstanceProcAddr) return -3;
    g_gipa = rr.getInstanceProcAddr;
    return 0;
}

static void log_pico_exts(PFN_xrGetInstanceProcAddr gipa) {
    PFN_xrEnumerateInstanceExtensionProperties en = 0;
    if (gipa(0, "xrEnumerateInstanceExtensionProperties", (PFN_xrVoidFunction*)&en) != 0 || !en) return;
    uint32_t n = 0;
    if (en(0, 0, &n, 0) != 0 || n == 0 || n > 512) return;
    static XrExtensionProperties props[512];
    for (uint32_t i = 0; i < n; i++) { memset(&props[i], 0, sizeof props[i]); props[i].type = 2; }
    if (en(0, n, &n, props) != 0) return;
    int hand = 0, ctrl = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (!strcmp(props[i].extensionName, "XR_PICO_hand_tracking")) hand = 1;
        if (!strcmp(props[i].extensionName, "XR_PICO_android_controller_function_ext_enable")) ctrl = 1;
    }
    LOGI("mark: runtime advertises %u exts, XR_PICO_hand_tracking=%d, XR_PICO_android_controller_function_ext_enable=%d", n, hand, ctrl);
}

// loaderInitInfo: the same XrLoaderInitInfoAndroidKHR VD passed to xrInitializeLoaderKHR.
// Returns 0 on success, negative step number on failure.
int vdhs_mark(const void* loaderInitInfo, uint64_t mask) {
    // Keep the handle open so the runtime (and its mark state) stays loaded for VD's loader.
    void* h = dlopen("libopenxr_forwardloader.so", RTLD_NOW);
    if (!h) { LOGI("mark: dlopen failed: %s", dlerror()); return -1; }
    PFN_xrInitializeLoaderKHR init = (PFN_xrInitializeLoaderKHR)dlsym(h, "xrInitializeLoaderKHR");
    PFN_xrNegotiateLoaderRuntimeInterface neg = (PFN_xrNegotiateLoaderRuntimeInterface)dlsym(h, "xrNegotiateLoaderRuntimeInterface");
    if (!init || !neg) { LOGI("mark: dlsym failed init=%p neg=%p", init, neg); return -2; }

    XrResult r = init(loaderInitInfo);
    LOGI("mark: forward xrInitializeLoaderKHR=%d", r);

    XrNegotiateLoaderInfo li = { 1, 1, sizeof li, 1, 1, XR_MAKE_VERSION(1, 0, 0), XR_MAKE_VERSION(1, 0x3ff, 0xfff) };
    XrNegotiateRuntimeRequest rr = { 3, 1, sizeof rr, 0, 0, 0 };
    r = neg(&li, &rr);
    LOGI("mark: negotiate=%d gipa=%p iface=%u", r, rr.getInstanceProcAddr, rr.runtimeInterfaceVersion);
    if (r != 0 || !rr.getInstanceProcAddr) return -3;

    PFN_xrMarkApiClass mark = 0;
    r = rr.getInstanceProcAddr(0, "xrMarkApiClass", (PFN_xrVoidFunction*)&mark);
    LOGI("mark: gipa(xrMarkApiClass)=%d fn=%p", r, mark);
    if (r != 0 || !mark) return -4;

    r = mark(mask);
    LOGI("mark: xrMarkApiClass(0x%llx)=%d", (unsigned long long)mask, r);
    log_pico_exts(rr.getInstanceProcAddr);
    return 0;
}

// Track Z: the runtime has already been initialised by VD's xrInitializeLoaderKHR.
// Negotiate only, then mark. Safe to call repeatedly; the mark is an OR of bits.
// Called by the compat layer (vdhs_layer.c) on VD's own thread, so negotiate
// sees a JVM-attached thread with the app classloader.
int vdhs_mark_now(void);
void* vdhs_runtime_gipa(void);
static int mark_now(void) {
    int rc = ensure_runtime();
    if (rc != 0) return rc;
    PFN_xrMarkApiClass mark = 0;
    XrResult r = g_gipa(0, "xrMarkApiClass", (PFN_xrVoidFunction*)&mark);
    if (r != 0 || !mark) { LOGI("mark_loaded: gipa(xrMarkApiClass)=%d fn=%p", r, mark); return -4; }
    r = mark(VDHS_MARK_MASK);
    LOGI("mark_loaded: xrMarkApiClass(0x%llx)=%d", (unsigned long long)VDHS_MARK_MASK, r);
    log_pico_exts(g_gipa);
    return 0;
}

int vdhs_mark_now(void) { return mark_now(); }
void* vdhs_runtime_gipa(void) { return ensure_runtime() == 0 ? (void*)g_gipa : 0; }
void vdhs_runtime_reset(void) { g_gipa = 0; }

// Managed entry (NativeBridge.MarkLoaded). The deferred background mark thread
// (mark_waiter) is gone: it SIGABRTed ~1/5 runs. With the
// compat layer the mark happens inside the enumerate hook on VD's thread, so
// this only tries once and never spawns a thread.
// Returns 0 when marked, -1 when the runtime is not up yet, <0 on error.
extern int vdhs_layer_active(void) __attribute__((weak));   // Track Z only
int vdhs_mark_loaded(void) {
    // With the layer's hooks in place the mark is done on VD's thread; a second
    // negotiate from the managed payload thread is exactly that risk.
    if (vdhs_layer_active && vdhs_layer_active()) { LOGI("mark_loaded: skipped (compat layer active)"); return 0; }
    return mark_now();
}

// Track Z: poll PICO's active input device. 0=HMD 1=controller 2=hand (E-023).
// Returns the value, or a negative code when unavailable.
int vdhs_active_input(uint64_t instance) {
    if (!instance) return -10;
    int rc = ensure_runtime();
    if (rc != 0) return rc;
    typedef XrResult (*PFN_xrGetActiveInputDeviceTypePico)(XrInstance, uint32_t*);
    PFN_xrGetActiveInputDeviceTypePico fn = 0;
    XrResult r = g_gipa(instance, "xrGetActiveInputDeviceTypePico", (PFN_xrVoidFunction*)&fn);
    if (r != 0 || !fn) return -11;
    uint32_t v = 0xffffffffu;
    r = fn(instance, &v);
    if (r != 0) return -12;
    return (int)v;
}
