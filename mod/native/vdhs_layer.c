// VD compat layer (see docs/ARCHITECTURE.md).
//
// Layer-shaped (hooked entry points + GIPA wrapper table), but installed as
// ShadowHook inline hooks on the exports of VD's OWN bundled Khronos loader
// (/data/app/.../lib/arm64/libopenxr_loader.so). No system library is touched.
//
// Hooked loader exports (offsets of the 714096 B loader in VD 1.34.22; logged
// as a cross-check only, the addresses come from the in-memory .dynsym):
//   xrEnumerateInstanceExtensionProperties 0x385f4  first call: force runtime
//                                                    load -> xrMarkApiClass -> VD's call
//   xrCreateInstance                       0x39530  mark (fallback), append controller
//                                                    ext, strip layer-only exts
//   xrGetInstanceProcAddr                  (sym)    extension-function wrapper table
//   xrCreateAction                         0x58cd4  remember "grip_pose"
//   xrGetActionStatePose                   0x5a054  isActive=false while PICO input==hand
//   xrGetSystemProperties                  0x54058  log supportsHandTracking (VD's feature gate)
//   xrEndFrame / xrPollEvent               (sym)    layer-list / passthrough event logs, permission fixes
//   xrCreateActionSpace / xrLocateSpace    (sym)    EyeGazeSpace log (gaze_log)
//
// Rollback: mode.txt=1 skips the layer (vdhs_payload.c).
#define _GNU_SOURCE
#include <link.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <android/log.h>

#include "shadowhook.h"
#include "vdhs_layer.h"

#define TAG "vdhs-layer"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

// ---------------------------------------------------------------- OpenXR subset
typedef int32_t XrResult;
typedef uint32_t XrBool32;
typedef uint64_t XrInstance, XrSession, XrAction, XrActionSet, XrPath, XrVersion;
typedef void (*PFN_xrVoidFunction)(void);
#define XR_ERROR_EXTENSION_NOT_PRESENT (-9)

typedef struct {
    uint32_t type; const void* next;
    char extensionName[128]; uint32_t extensionVersion;
} XrExtensionProperties;

typedef struct {
    char applicationName[128]; uint32_t applicationVersion;
    char engineName[128]; uint32_t engineVersion;
    XrVersion apiVersion;
} XrApplicationInfo;

typedef struct {
    uint32_t type; const void* next;
    uint64_t createFlags;
    XrApplicationInfo applicationInfo;
    uint32_t enabledApiLayerCount; const char* const* enabledApiLayerNames;
    uint32_t enabledExtensionCount; const char* const* enabledExtensionNames;
} XrInstanceCreateInfo;

typedef struct {
    uint32_t type; const void* next;
    char actionName[64]; int32_t actionType;
    uint32_t countSubactionPaths; const XrPath* subactionPaths;
    char localizedActionName[128];
} XrActionCreateInfo;

typedef struct { uint32_t type; const void* next; XrAction action; XrPath subactionPath; } XrActionStateGetInfo;
typedef struct { uint32_t type; void* next; XrBool32 isActive; } XrActionStatePose;

typedef XrResult (*PFN_enum)(const char*, uint32_t, uint32_t*, XrExtensionProperties*);
typedef XrResult (*PFN_create_instance)(const XrInstanceCreateInfo*, XrInstance*);
typedef XrResult (*PFN_gipa)(XrInstance, const char*, PFN_xrVoidFunction*);
typedef XrResult (*PFN_create_action)(XrActionSet, const XrActionCreateInfo*, XrAction*);
typedef XrResult (*PFN_get_pose)(XrSession, const XrActionStateGetInfo*, XrActionStatePose*);
typedef XrResult (*PFN_active_input)(XrInstance, uint32_t*);
struct XrBaseOutStructure;
typedef XrResult (*PFN_get_system_props)(XrInstance, uint64_t, struct XrBaseOutStructure*);

// vdhs_payload.c
void* vdhs_resolve_mapped_symbol(const char* lib, const char* name);

// VD ships the loader in its app lib dir; the system has no file of this name
// under /lib/arm64/, so the suffix identifies VD's copy.
static const char kLoaderNeedle[] = "/lib/arm64/libopenxr_loader.so";
static const char kControllerExt[] = "XR_PICO_android_controller_function_ext_enable";

// ---------------------------------------------------------------- state
static PFN_enum            g_orig_enum;
static PFN_create_instance g_orig_create_instance;
static PFN_gipa            g_orig_gipa;
static PFN_create_action   g_orig_create_action;
static PFN_get_pose        g_orig_get_pose;
static PFN_get_system_props g_orig_get_system_props;

static volatile int g_hooks_ok;
static volatile int g_marked;
static volatile XrInstance g_instance;
static int64_t g_t0;

#define MAX_GRIP 8
static XrAction g_grip[MAX_GRIP];
static volatile int g_grip_n;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// Extensions the layer serves itself (§2.2/§2.3): advertised in enumerate,
// stripped before the runtime's xrCreateInstance. Off until step 3/4 lands so
// VD does not enable features the layer cannot serve yet.
static const char* const kLayerExts[] = { "XR_FB_hand_tracking_mesh", "XR_FB_hand_tracking_aim" };
enum { kLayerExtCount = (int)(sizeof kLayerExts / sizeof kLayerExts[0]) };
static const int g_advertise_layer_exts = 1;
static const char kHandExt[] = "XR_EXT_hand_tracking";
// Xenko.OpenXR.Extension values (extracted/decompiled/.../Extension.cs).
enum { kExtEnumHandTracking = 27, kExtEnumFbAim = 50, kExtEnumFbMesh = 52 };

// ---------------------------------------------------------------- §2.4 passthrough diagnostics
// Log-only wrappers around VD's Hands passthrough path (PassthroughPortals.cs):
// return codes plus the geometry-instance pose/scale. Behaviour is unchanged.
typedef struct { float x, y, z, w; } XrQuaternionf;
typedef struct { float x, y, z; } XrVector3f;
typedef struct { XrQuaternionf orientation; XrVector3f position; } XrPosef;
typedef uint64_t XrHandle;

typedef struct { uint32_t type; const void* next; uint64_t flags; } XrPassthroughCreateInfoFB;
typedef struct { uint32_t type; const void* next; XrHandle passthrough; uint64_t flags; int32_t purpose; } XrPassthroughLayerCreateInfoFB;
typedef struct { uint32_t type; const void* next; uint64_t flags; XrHandle space; XrHandle layerHandle; } XrCompositionLayerPassthroughFB;
typedef struct { uint32_t type; const void* next; uint64_t layerFlags; XrHandle space; } XrCompositionLayerBaseHeader;
typedef struct {
    uint32_t type; const void* next; int64_t displayTime; int32_t environmentBlendMode;
    uint32_t layerCount; const XrCompositionLayerBaseHeader* const* layers;
} XrFrameEndInfo;
typedef struct {
    uint32_t type; const void* next; uint64_t flags; int32_t windingOrder;
    uint32_t vertexCount; const XrVector3f* vertexBuffer;
    uint32_t triangleCount; const uint32_t* indexBuffer;
} XrTriangleMeshCreateInfoFB;
typedef struct {
    uint32_t type; const void* next; XrHandle layer, mesh, baseSpace;
    XrPosef pose; XrVector3f scale;
} XrGeometryInstanceCreateInfoFB;
typedef struct {
    uint32_t type; const void* next; XrHandle baseSpace; int64_t time;
    XrPosef pose; XrVector3f scale;
} XrGeometryInstanceTransformFB;

typedef XrResult (*PFN_create_pt)(XrSession, const XrPassthroughCreateInfoFB*, XrHandle*);
typedef XrResult (*PFN_create_pt_layer)(XrSession, const XrPassthroughLayerCreateInfoFB*, XrHandle*);
typedef XrResult (*PFN_create_tri_mesh)(XrSession, const XrTriangleMeshCreateInfoFB*, XrHandle*);
typedef XrResult (*PFN_create_geom)(XrSession, const XrGeometryInstanceCreateInfoFB*, XrHandle*);
typedef XrResult (*PFN_geom_set_xform)(XrHandle, const XrGeometryInstanceTransformFB*);

static PFN_create_pt       g_next_create_pt;
static PFN_create_tri_mesh g_next_create_tri_mesh;
static PFN_create_geom     g_next_create_geom;
static PFN_geom_set_xform  g_next_geom_set_xform;

static XrResult w_create_pt(XrSession s, const XrPassthroughCreateInfoFB* ci, XrHandle* out) {
    XrResult r = g_next_create_pt(s, ci, out);
    LOGI("pt: xrCreatePassthroughFB flags=0x%llx r=%d h=0x%llx", (unsigned long long)(ci ? ci->flags : 0), r,
         (unsigned long long)(r == 0 && out ? *out : 0));
    return r;
}

// PICO runtime (libpxrruntime oxr_passthrough_layer_pause/resume 0x5fe8c/0x5fd48,
// destroy 0x5fc64): pausing, resuming or destroying ANY layer with purpose
// RECONSTRUCTION(0) or PROJECTED(1) sets one global background see-through
// flag (set_background_seethrough_visibility). VD pauses its projected (hands)
// layer right after creating the reconstruction layer for the Passthrough
// environment, which turned the environment's see-through off. The layer keeps
// the per-layer running state and re-resumes a still-running layer afterwards.
#define MAX_PT_LAYERS 16
static struct { XrHandle h; int32_t purpose; int running; } g_pt_layers[MAX_PT_LAYERS];
static XrHandle g_pt_passthrough[MAX_PT_LAYERS];   // parallel to g_pt_layers: parent passthrough

static int pt_find(XrHandle h) {
    for (int i = 0; i < MAX_PT_LAYERS; i++) if (g_pt_layers[i].h == h) return i;
    return -1;
}
static pthread_mutex_t g_pt_lock = PTHREAD_MUTEX_INITIALIZER;
static PFN_create_pt_layer g_next_create_pt_layer;

typedef XrResult (*PFN_pt_handle)(XrHandle);
static PFN_pt_handle g_next_pt_start, g_next_pt_pause, g_next_pt_layer_resume, g_next_pt_layer_pause;
static PFN_pt_handle g_next_destroy_geom, g_next_destroy_pt_layer;

static void pt_track(XrHandle h, int32_t purpose, int running) {
    pthread_mutex_lock(&g_pt_lock);
    int free_i = -1;
    for (int i = 0; i < MAX_PT_LAYERS; i++) {
        if (g_pt_layers[i].h == h) { g_pt_layers[i].running = running; free_i = -2; break; }
        if (!g_pt_layers[i].h && free_i == -1) free_i = i;
    }
    if (free_i >= 0) { g_pt_layers[free_i].h = h; g_pt_layers[free_i].purpose = purpose; g_pt_layers[free_i].running = running; }
    pthread_mutex_unlock(&g_pt_lock);
}

static void pt_set_running(XrHandle h, int running, int forget) {
    pthread_mutex_lock(&g_pt_lock);
    for (int i = 0; i < MAX_PT_LAYERS; i++)
        if (g_pt_layers[i].h == h) {
            g_pt_layers[i].running = running;
            if (forget) g_pt_layers[i].h = 0;
        }
    pthread_mutex_unlock(&g_pt_lock);
}

// After `gone` stopped: resume the first other running background-purpose layer
// so the global see-through flag matches it again.
static void pt_restore_background(XrHandle gone) {
    XrHandle keep = 0;
    int32_t purpose = -1;
    pthread_mutex_lock(&g_pt_lock);
    for (int i = 0; i < MAX_PT_LAYERS && !keep; i++)
        if (g_pt_layers[i].h && g_pt_layers[i].h != gone && g_pt_layers[i].running && g_pt_layers[i].purpose <= 1) {
            keep = g_pt_layers[i].h;
            purpose = g_pt_layers[i].purpose;
        }
    pthread_mutex_unlock(&g_pt_lock);
    if (!keep || !g_next_pt_layer_resume) return;
    XrResult r = g_next_pt_layer_resume(keep);
    LOGI("pt: re-resumed h=0x%llx purpose=%d after h=0x%llx stopped r=%d",
         (unsigned long long)keep, purpose, (unsigned long long)gone, r);
}

// Optional (hand_gesture.txt pt_style=1, default off since session 7): set
// textureOpacityFactor=1 on the projected layer. Suspected of breaking the
// Passthrough environment in session 6; kept only for A/B testing.
typedef struct { float x, y, z, w; } XrColor4f;
typedef struct { uint32_t type; const void* next; float textureOpacityFactor; XrColor4f edgeColor; } XrPassthroughStyleFB;
typedef XrResult (*PFN_pt_style_fn)(XrHandle, const XrPassthroughStyleFB*);
static PFN_pt_style_fn g_pt_style_direct;

static XrResult w_create_pt_layer(XrSession s, const XrPassthroughLayerCreateInfoFB* ci, XrHandle* out) {
    XrResult r = g_next_create_pt_layer(s, ci, out);
    LOGI("pt: xrCreatePassthroughLayerFB purpose=%d flags=0x%llx r=%d h=0x%llx", ci ? ci->purpose : -1,
         (unsigned long long)(ci ? ci->flags : 0), r, (unsigned long long)(r == 0 && out ? *out : 0));
    if (r != 0 || !out || !ci) return r;
    pt_track(*out, ci->purpose, (int)(ci->flags & 1));   // XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB
    pthread_mutex_lock(&g_pt_lock);
    int vi = pt_find(*out);
    if (vi >= 0) g_pt_passthrough[vi] = ci->passthrough;
    pthread_mutex_unlock(&g_pt_lock);
    if (ci->purpose == 1 && vdhs_hand_cfg("pt_style") > 0) {   // PROJECTED
        if (!g_pt_style_direct && g_instance)
            g_orig_gipa(g_instance, "xrPassthroughLayerSetStyleFB", (PFN_xrVoidFunction*)&g_pt_style_direct);
        if (g_pt_style_direct) {
            XrPassthroughStyleFB st;
            memset(&st, 0, sizeof st);
            st.type = 1000118020u;   // XR_TYPE_PASSTHROUGH_STYLE_FB
            st.textureOpacityFactor = 1.0f;
            LOGI("pt: projected layer style opacity=1 r=%d", g_pt_style_direct(*out, &st));
        }
    }
    return r;
}

static void pt_extra_mirror(XrHandle vd_layer, int op);

static XrResult w_pt_layer_resume(XrHandle h) {
    XrResult r = g_next_pt_layer_resume(h);
    LOGI("pt: xrPassthroughLayerResumeFB h=0x%llx r=%d", (unsigned long long)h, r);
    if (r == 0) { pt_set_running(h, 1, 0); pt_extra_mirror(h, 1); }
    return r;
}

static XrResult w_pt_layer_pause(XrHandle h) {
    XrResult r = g_next_pt_layer_pause(h);
    LOGI("pt: xrPassthroughLayerPauseFB h=0x%llx r=%d", (unsigned long long)h, r);
    if (r == 0) {
        pt_set_running(h, 0, 0);
        pt_extra_mirror(h, 0);
        if (vdhs_hand_cfg("pt_bg_fix") > 0) pt_restore_background(h);
    }
    return r;
}

static XrResult w_destroy_pt_layer(XrHandle h) {
    pt_extra_mirror(h, 2);
    XrResult r = g_next_destroy_pt_layer(h);
    LOGI("pt: xrDestroyPassthroughLayerFB h=0x%llx r=%d", (unsigned long long)h, r);
    if (r == 0) {
        pt_set_running(h, 0, 1);
        if (vdhs_hand_cfg("pt_bg_fix") > 0) pt_restore_background(h);
    }
    return r;
}

// One geometry per passthrough layer on PICO: oxr_session_create_geometry_instance
// (libpxrruntime 0x60658) memcpy's the mesh into record[layer index] and
// set_transform (0x607f0) writes the same record, so VD's second hand on the
// shared projected layer overwrites the first. When a projected layer already
// has a live geometry, the layer creates an extra projected layer on the same
// passthrough, puts the geometry there, mirrors resume/pause/destroy of VD's
// layer to it and submits it right after VD's layer in xrEndFrame
// (hand_gesture.txt pt_split, default 1).
#define MAX_PT_EXTRA 4
typedef struct {
    XrHandle vd_layer, layer, geom;
    XrCompositionLayerPassthroughFB copy;   // per-frame submitted copy of VD's layer
} pt_extra;
static pt_extra g_pt_extra[MAX_PT_EXTRA];
#define MAX_PT_GEOM 16
static struct { XrHandle geom, vd_layer; int extra; } g_pt_geom[MAX_PT_GEOM];   // extra: index or -1

static int pt_live_on(XrHandle vd_layer) {
    int n = 0;
    for (int i = 0; i < MAX_PT_GEOM; i++) if (g_pt_geom[i].geom && g_pt_geom[i].vd_layer == vd_layer && g_pt_geom[i].extra < 0) n++;
    return n;
}

static void pt_geom_add(XrHandle geom, XrHandle vd_layer, int extra) {
    for (int i = 0; i < MAX_PT_GEOM; i++)
        if (!g_pt_geom[i].geom) { g_pt_geom[i].geom = geom; g_pt_geom[i].vd_layer = vd_layer; g_pt_geom[i].extra = extra; return; }
}

// Under g_pt_lock. Returns an extra slot for vd_layer without a geometry,
// creating its layer if needed; -1 on failure.
static int pt_extra_get(XrSession s, XrHandle vd_layer) {
    int free_i = -1;
    for (int i = 0; i < MAX_PT_EXTRA; i++) {
        if (g_pt_extra[i].layer && g_pt_extra[i].vd_layer == vd_layer && !g_pt_extra[i].geom) return i;
        if (!g_pt_extra[i].layer && free_i < 0) free_i = i;
    }
    int vi = pt_find(vd_layer);
    if (free_i < 0 || vi < 0 || !g_pt_passthrough[vi]) return -1;
    XrPassthroughLayerCreateInfoFB ci;
    memset(&ci, 0, sizeof ci);
    ci.type = 1000118002u;   // XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB
    ci.passthrough = g_pt_passthrough[vi];
    ci.flags = g_pt_layers[vi].running ? 1 : 0;
    ci.purpose = 1;          // PROJECTED
    XrHandle h = 0;
    XrResult r = g_next_create_pt_layer(s, &ci, &h);
    LOGI("pt: extra projected layer for vd=0x%llx r=%d h=0x%llx running=%d",
         (unsigned long long)vd_layer, r, (unsigned long long)h, (int)ci.flags);
    if (r != 0 || !h) return -1;
    g_pt_extra[free_i].vd_layer = vd_layer;
    g_pt_extra[free_i].layer = h;
    g_pt_extra[free_i].geom = 0;
    return free_i;
}

// Mirror VD's resume/pause/destroy of vd_layer to its extra layers.
// op: 0 pause, 1 resume, 2 destroy.
static void pt_extra_mirror(XrHandle vd_layer, int op) {
    XrHandle hs[MAX_PT_EXTRA];
    int n = 0;
    pthread_mutex_lock(&g_pt_lock);
    for (int i = 0; i < MAX_PT_EXTRA; i++)
        if (g_pt_extra[i].layer && g_pt_extra[i].vd_layer == vd_layer) {
            hs[n++] = g_pt_extra[i].layer;
            if (op == 2) memset(&g_pt_extra[i], 0, sizeof g_pt_extra[i]);
        }
    pthread_mutex_unlock(&g_pt_lock);
    for (int i = 0; i < n; i++) {
        XrResult r = op == 0 ? g_next_pt_layer_pause(hs[i]) : op == 1 ? g_next_pt_layer_resume(hs[i]) : g_next_destroy_pt_layer(hs[i]);
        LOGI("pt: extra layer h=0x%llx %s r=%d", (unsigned long long)hs[i], op == 0 ? "pause" : op == 1 ? "resume" : "destroy", r);
    }
}

static XrResult w_create_tri_mesh(XrSession s, const XrTriangleMeshCreateInfoFB* ci, XrHandle* out) {
    XrResult r = g_next_create_tri_mesh(s, ci, out);
    LOGI("pt: xrCreateTriangleMeshFB verts=%u tris=%u winding=%d flags=0x%llx r=%d h=0x%llx",
         ci ? ci->vertexCount : 0, ci ? ci->triangleCount : 0, ci ? ci->windingOrder : -1,
         (unsigned long long)(ci ? ci->flags : 0), r, (unsigned long long)(r == 0 && out ? *out : 0));
    return r;
}

static XrResult w_create_geom(XrSession s, const XrGeometryInstanceCreateInfoFB* ci, XrHandle* out) {
    if (!ci || !out || vdhs_hand_cfg("pt_split") <= 0) {
        XrResult r = g_next_create_geom(s, ci, out);
        if (ci)
            LOGI("pt: xrCreateGeometryInstanceFB layer=0x%llx mesh=0x%llx r=%d h=0x%llx", (unsigned long long)ci->layer,
                 (unsigned long long)ci->mesh, r, (unsigned long long)(r == 0 && out ? *out : 0));
        return r;
    }
    // A second live geometry on the same projected layer goes to an extra layer.
    int ei = -1;
    pthread_mutex_lock(&g_pt_lock);
    int vi = pt_find(ci->layer);
    if (vi >= 0 && g_pt_layers[vi].purpose == 1 && pt_live_on(ci->layer) >= 1) ei = pt_extra_get(s, ci->layer);
    pthread_mutex_unlock(&g_pt_lock);

    XrGeometryInstanceCreateInfoFB c = *ci;
    if (ei >= 0) c.layer = g_pt_extra[ei].layer;
    XrResult r = g_next_create_geom(s, &c, out);
    LOGI("pt: xrCreateGeometryInstanceFB layer=0x%llx%s mesh=0x%llx r=%d h=0x%llx", (unsigned long long)c.layer,
         ei >= 0 ? " (extra)" : "", (unsigned long long)ci->mesh, r, (unsigned long long)(r == 0 ? *out : 0));
    if (r == 0) {
        pthread_mutex_lock(&g_pt_lock);
        if (ei >= 0) g_pt_extra[ei].geom = *out;
        pt_geom_add(*out, ci->layer, ei);
        pthread_mutex_unlock(&g_pt_lock);
    }
    return r;
}

static XrResult w_destroy_geom(XrHandle g) {
    XrResult r = g_next_destroy_geom(g);
    LOGI("pt: xrDestroyGeometryInstanceFB h=0x%llx r=%d", (unsigned long long)g, r);
    pthread_mutex_lock(&g_pt_lock);
    for (int i = 0; i < MAX_PT_GEOM; i++)
        if (g_pt_geom[i].geom == g) {
            int ei = g_pt_geom[i].extra;
            if (ei >= 0 && g_pt_extra[ei].geom == g) g_pt_extra[ei].geom = 0;   // layer kept for reuse
            memset(&g_pt_geom[i], 0, sizeof g_pt_geom[i]);
        }
    pthread_mutex_unlock(&g_pt_lock);
    return r;
}

// xrEndFrame: after each submitted XrCompositionLayerPassthroughFB whose layer
// has extra layers with a geometry, insert copies pointing at those layers.
// Returns the layer array to submit (fi's own when nothing is added).
#define MAX_SUBMIT_LAYERS 32
static const XrCompositionLayerBaseHeader* g_submit[MAX_SUBMIT_LAYERS];
static uint32_t pt_extra_layers(const XrFrameEndInfo* fi, const XrCompositionLayerBaseHeader* const** out) {
    *out = fi->layers;
    if (!fi->layers || !fi->layerCount || fi->layerCount >= MAX_SUBMIT_LAYERS) return fi->layerCount;
    int any = 0;
    for (int e = 0; e < MAX_PT_EXTRA; e++) if (g_pt_extra[e].layer && g_pt_extra[e].geom) any = 1;
    if (!any) return fi->layerCount;
    uint32_t n = 0;
    pthread_mutex_lock(&g_pt_lock);
    for (uint32_t i = 0; i < fi->layerCount && n < MAX_SUBMIT_LAYERS; i++) {
        const XrCompositionLayerBaseHeader* L = fi->layers[i];
        g_submit[n++] = L;
        if (!L || L->type != 1000118003u) continue;   // XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB
        const XrCompositionLayerPassthroughFB* P = (const XrCompositionLayerPassthroughFB*)L;
        for (int e = 0; e < MAX_PT_EXTRA && n < MAX_SUBMIT_LAYERS; e++) {
            pt_extra* x = &g_pt_extra[e];
            if (!x->layer || !x->geom || x->vd_layer != P->layerHandle) continue;
            x->copy = *P;   // same flags / space / ColorScaleBias next chain
            x->copy.layerHandle = x->layer;
            g_submit[n++] = (const XrCompositionLayerBaseHeader*)&x->copy;
        }
    }
    pthread_mutex_unlock(&g_pt_lock);
    if (n == fi->layerCount) return n;
    *out = g_submit;
    return n;
}

// Rate-limited: first 8 calls, then every ~2 s, plus every non-success.
static XrResult w_geom_set_xform(XrHandle g, const XrGeometryInstanceTransformFB* t) {
    static volatile int calls;
    static int64_t last_ms;
    XrResult r = g_next_geom_set_xform(g, t);
    int n = __atomic_add_fetch(&calls, 1, __ATOMIC_RELAXED);
    int64_t tm = now_ms();
    if (t && (n <= 8 || r != 0 || tm - last_ms >= 2000)) {
        last_ms = tm;
        LOGI("pt: SetTransform#%d g=0x%llx p=(%.3f,%.3f,%.3f) q=(%.2f,%.2f,%.2f,%.2f) s=(%.3f,%.3f,%.3f) r=%d",
             n, (unsigned long long)g, t->pose.position.x, t->pose.position.y, t->pose.position.z,
             t->pose.orientation.x, t->pose.orientation.y, t->pose.orientation.z, t->pose.orientation.w,
             t->scale.x, t->scale.y, t->scale.z, r);
    }
    return r;
}

// Log-only: passthrough start/pause, geometry destroy and style (handle -> result).
typedef XrResult (*PFN_pt_style)(XrHandle, const void*);
static PFN_pt_style  g_next_pt_layer_style;
#define PT_LOG1(fn, next) \
    static XrResult fn(XrHandle h) { XrResult r = next(h); \
        LOGI("pt: %s h=0x%llx r=%d", &#fn[2], (unsigned long long)h, r); return r; }
PT_LOG1(w_xrPassthroughStartFB,        g_next_pt_start)
PT_LOG1(w_xrPassthroughPauseFB,        g_next_pt_pause)
#undef PT_LOG1
static XrResult w_pt_layer_style(XrHandle h, const void* style) {
    XrResult r = g_next_pt_layer_style(h, style);
    LOGI("pt: xrPassthroughLayerSetStyleFB h=0x%llx r=%d", (unsigned long long)h, r);
    return r;
}

// GIPA wrapper table. A row maps an OpenXR function name to the layer's
// wrapper and the slot that receives the downstream (loader/runtime) function.
// Step 4 (aim/mesh) appends rows here (xrLocateHandJointsEXT, xrGetHandMeshFB).
typedef struct { const char* name; PFN_xrVoidFunction wrapper; PFN_xrVoidFunction* next; } gipa_entry;
#define ROW(n, w, slot) { n, (PFN_xrVoidFunction)(w), (PFN_xrVoidFunction*)&(slot) }
static gipa_entry g_gipa_table[] = {
    ROW("xrCreatePassthroughFB",            w_create_pt,       g_next_create_pt),
    ROW("xrCreatePassthroughLayerFB",       w_create_pt_layer, g_next_create_pt_layer),
    ROW("xrCreateTriangleMeshFB",           w_create_tri_mesh, g_next_create_tri_mesh),
    ROW("xrCreateGeometryInstanceFB",       w_create_geom,     g_next_create_geom),
    ROW("xrGeometryInstanceSetTransformFB", w_geom_set_xform,  g_next_geom_set_xform),
    ROW("xrPassthroughStartFB",             w_xrPassthroughStartFB,        g_next_pt_start),
    ROW("xrPassthroughPauseFB",             w_xrPassthroughPauseFB,        g_next_pt_pause),
    ROW("xrPassthroughLayerResumeFB",       w_pt_layer_resume,             g_next_pt_layer_resume),
    ROW("xrPassthroughLayerPauseFB",        w_pt_layer_pause,              g_next_pt_layer_pause),
    ROW("xrPassthroughLayerSetStyleFB",     w_pt_layer_style,              g_next_pt_layer_style),
    ROW("xrDestroyGeometryInstanceFB",      w_destroy_geom,                g_next_destroy_geom),
    ROW("xrDestroyPassthroughLayerFB",      w_destroy_pt_layer,            g_next_destroy_pt_layer),
    { NULL, NULL, NULL },   // sentinel
};
#undef ROW

#define TS() ((long long)(now_ms() - g_t0))

int vdhs_layer_active(void) { return g_hooks_ok; }
int vdhs_layer_marked(void) { return g_marked; }

// Mark on the calling (VD) thread; the runtime must already be loaded.
static void try_mark(const char* where) {
    if (g_marked) return;
    pthread_mutex_lock(&g_lock);
    if (!g_marked) {
        int rc = vdhs_mark_now();
        if (rc == 0) g_marked = 1;
        LOGI("mark from %s: rc=%d (+%lld ms)", where, rc, TS());
    }
    pthread_mutex_unlock(&g_lock);
}

// ---------------------------------------------------------------- hooks
static XrResult hk_enum(const char* layer, uint32_t cap, uint32_t* count, XrExtensionProperties* props) {
    static int logged;
    if (!g_marked && !layer) {
        // Force the loader to load the runtime (it does so lazily on the first
        // enumerate), then mark before VD's own call sees the list.
        uint32_t n = 0;
        XrResult r0 = g_orig_enum(NULL, 0, &n, NULL);
        LOGI("enum: pre-call r=%d n=%u (+%lld ms)", r0, n, TS());
        try_mark("enumerate");
    }
    if (layer || !g_advertise_layer_exts) {
        XrResult r = g_orig_enum(layer, cap, count, props);
        if (!logged && count && cap == 0) { logged = 1; LOGI("enum: VD count=%u r=%d", *count, r); }
        return r;
    }

    // Append layer-served extensions to the runtime list.
    uint32_t n = 0;
    XrResult r = g_orig_enum(NULL, 0, &n, NULL);
    if (r != 0) return r;
    uint32_t total = n + kLayerExtCount;
    if (count) *count = total;
    if (cap == 0) return 0;
    if (cap < total) return -11;   // XR_ERROR_SIZE_INSUFFICIENT
    r = g_orig_enum(NULL, cap, &n, props);
    if (r != 0) return r;
    for (int i = 0; i < kLayerExtCount; i++) {
        XrExtensionProperties* p = &props[n + i];
        memset(p->extensionName, 0, sizeof p->extensionName);
        strncpy(p->extensionName, kLayerExts[i], sizeof p->extensionName - 1);
        p->extensionVersion = 1;
    }
    return 0;
}

static int is_layer_ext(const char* s) {
    for (int i = 0; i < kLayerExtCount; i++) if (!strcmp(s, kLayerExts[i])) return 1;
    return 0;
}

static XrResult hk_create_instance(const XrInstanceCreateInfo* ci, XrInstance* out) {
    try_mark("createInstance");   // no-op when enumerate already marked
    if (!ci) return g_orig_create_instance(ci, out);

    uint32_t n = ci->enabledExtensionCount;
    const char** names = (const char**)calloc((size_t)n + 1, sizeof *names);
    if (!names) return g_orig_create_instance(ci, out);
    uint32_t m = 0;
    int has_ctrl = 0, stripped = 0;
    for (uint32_t i = 0; i < n; i++) {
        const char* s = ci->enabledExtensionNames[i];
        if (!s) continue;
        if (is_layer_ext(s)) { stripped++; continue; }
        if (!strcmp(s, kControllerExt)) has_ctrl = 1;
        names[m++] = s;
    }

    // Order matters for the fallbacks below: [VD list][hand ext][controller ext].
    int has_hand = 0;
    for (uint32_t i = 0; i < m; i++) if (!strcmp(names[i], kHandExt)) has_hand = 1;
    int hand_added = !has_hand;
    if (hand_added) names[m++] = kHandExt;
    int added = !has_ctrl && g_marked;
    if (added) names[m++] = kControllerExt;

    XrInstanceCreateInfo copy = *ci;
    copy.enabledExtensionCount = m;
    copy.enabledExtensionNames = names;
    XrResult r = g_orig_create_instance(&copy, out);
    LOGI("createInstance: r=%d exts %u->%u (vd_had_ctrl=%d added=%d stripped=%d) (+%lld ms)",
         r, n, m, has_ctrl, added, stripped, TS());

    // The loader caches the runtime's extension list when it loads the runtime
    // (runtime_interface.cpp SetSupportedExtensions). If that happened before
    // the mark, the controller extension is rejected with
    // XR_ERROR_EXTENSION_NOT_PRESENT; the failure path unloads the runtime, so
    // one retry reloads it and re-reads the (now marked) list.
    if (r == XR_ERROR_EXTENSION_NOT_PRESENT && added) {
        vdhs_runtime_reset();   // forward loader may be re-mapped by the reload
        r = g_orig_create_instance(&copy, out);
        LOGI("createInstance: retry after runtime reload r=%d (+%lld ms)", r, TS());
    }
    // Never leave VD without an instance because of the layer: VD crashes on a
    // failed xrCreateInstance (ReleasePassthrough NRE, observed).
    if (r == XR_ERROR_EXTENSION_NOT_PRESENT && added) {
        copy.enabledExtensionCount = --m;
        r = g_orig_create_instance(&copy, out);
        LOGE("createInstance: controller ext still rejected; without it r=%d", r);
    }
    if (r == XR_ERROR_EXTENSION_NOT_PRESENT && hand_added) {
        copy.enabledExtensionCount = --m;
        hand_added = 0;
        r = g_orig_create_instance(&copy, out);
        LOGE("createInstance: hand ext rejected; VD list only r=%d", r);
    }
    if (r == 0 && out) g_instance = *out;
    LOGI("createInstance: final r=%d instance=0x%llx hand_ext=%d", r,
         (unsigned long long)(r == 0 && out ? *out : 0), has_hand || hand_added);
    free(names);

    // Plan A (§4.3a): VD has already stored the whitelist-filtered set in
    // XR._supportedExtensions (XR.cs:1159); add hand tracking to it so
    // Session.InitializeHandTracking runs. 52 (mesh) only when the layer can
    // serve xrGetHandMeshFB, otherwise VD's CheckError would throw.
    if (r == 0 && (has_hand || hand_added)) {
        int32_t vals[3];
        int nv = 0;
        vals[nv++] = kExtEnumHandTracking;
        vals[nv++] = kExtEnumFbAim;
        if (vdhs_hand_mesh_ready()) vals[nv++] = kExtEnumFbMesh;
        int irc = vdhs_hand_inject_managed(vals, nv);
        LOGI("createInstance: managed inject rc=%d (%d values, mesh=%d) (+%lld ms)", irc, nv, nv == 3, TS());
    }
    if (r == 0) vdhs_hand_capture_settings();
    return r;
}

static XrResult hk_gipa(XrInstance inst, const char* name, PFN_xrVoidFunction* fn) {
    // Functions the layer implements itself: the runtime would answer
    // FUNCTION_UNSUPPORTED and VD's GetDelegate CheckError()s (XR.cs:641-649).
    if (inst && name && fn && vdhs_hand_gipa_self(name, fn)) return 0;
    XrResult r = g_orig_gipa(inst, name, fn);
    if (r != 0 || !name || !fn || !*fn) return r;
    if (vdhs_hand_gipa_wrap(name, fn)) return r;
    for (gipa_entry* e = g_gipa_table; e->name; e++) {
        if (strcmp(e->name, name) != 0) continue;
        *e->next = *fn;
        *fn = e->wrapper;
        LOGI("gipa: wrapped %s", name);
        break;
    }
    return r;
}

// Log-only: VD gates all hand code on Features.HandTracking, i.e.
// XrSystemHandTrackingPropertiesEXT.supportsHandTracking (XR.cs:397).
typedef struct XrBaseOutStructure { uint32_t type; void* next; } XrBaseOutStructure;
typedef struct { uint32_t type; void* next; XrBool32 supportsHandTracking; } XrSystemHandTrackingPropertiesEXT;
static XrResult hk_get_system_props(XrInstance inst, uint64_t sys, XrBaseOutStructure* props) {
    XrResult r = g_orig_get_system_props(inst, sys, props);
    for (XrBaseOutStructure* s = props ? (XrBaseOutStructure*)props->next : NULL; s; s = (XrBaseOutStructure*)s->next) {
        if (s->type == 1000051000u)        // XR_TYPE_SYSTEM_HAND_TRACKING_PROPERTIES_EXT
            LOGI("systemProps: r=%d supportsHandTracking=%u", r, ((XrSystemHandTrackingPropertiesEXT*)s)->supportsHandTracking);
        else if (s->type == 1000030001u)   // XR_TYPE_SYSTEM_EYE_GAZE_INTERACTION_PROPERTIES_EXT
            LOGI("systemProps: r=%d supportsEyeGazeInteraction=%u", r, ((XrSystemHandTrackingPropertiesEXT*)s)->supportsHandTracking);
    }
    return r;
}

// Log-only: xrEndFrame layer list. Logged when the layer signature (count,
// blend mode, types, positions of passthrough layers) changes, plus ~every 10 s.
typedef XrResult (*PFN_end_frame)(XrSession, const XrFrameEndInfo*);
static PFN_end_frame g_orig_end_frame;

static XrResult hk_end_frame(XrSession s, const XrFrameEndInfo* fi_vd) {
    // Extra projected layers (second hand) are submitted right after VD's.
    XrFrameEndInfo fi_copy;
    const XrFrameEndInfo* fi = fi_vd;
    if (fi_vd) {
        const XrCompositionLayerBaseHeader* const* layers;
        uint32_t n = pt_extra_layers(fi_vd, &layers);
        if (layers != fi_vd->layers) {
            fi_copy = *fi_vd;
            fi_copy.layerCount = n;
            fi_copy.layers = layers;
            fi = &fi_copy;
        }
    }
    // Zero-copy alpha hole: record the VR projection layer's views / swapchains
    // and OR 0x6 into its flags before the compositor sees the frame.
    vdhs_pt_on_end_frame(fi);
    XrResult r = g_orig_end_frame(s, fi);
    // Permission-name fixes need the managed UI tabs, which are built after the
    // instance exists; retry from VD's render thread (Mono-attached P/Invoke)
    // every 2 s, and keep checking InputTab in case the tab is rebuilt.
    {
        static int64_t next_ms;
        static int perm_done;
        int64_t t = now_ms();
        if (t >= next_ms) {
            next_ms = t + 2000;
            if (!perm_done) perm_done = vdhs_hand_fix_permissions() == 0;
            vdhs_hand_fix_inputtab();
        }
    }
    if (!fi) return r;
    static uint64_t last_sig;
    static int64_t last_ms;
    static int last_r;
    uint64_t sig = (uint64_t)fi->layerCount * 1000003u + (uint64_t)fi->environmentBlendMode * 7919u;
    for (uint32_t i = 0; i < fi->layerCount && fi->layers; i++)
        if (fi->layers[i]) sig = sig * 31u + fi->layers[i]->type + (fi->layers[i]->layerFlags << 8);
    int64_t t = now_ms();
    if (sig == last_sig && r == last_r && t - last_ms < 10000) return r;
    last_sig = sig; last_ms = t; last_r = r;
    char buf[900];
    int n = snprintf(buf, sizeof buf, "frame: endFrame r=%d blend=%d layers=%u:", r, fi->environmentBlendMode, fi->layerCount);
    for (uint32_t i = 0; i < fi->layerCount && fi->layers && n < (int)sizeof buf - 80; i++) {
        const XrCompositionLayerBaseHeader* L = fi->layers[i];
        if (!L) { n += snprintf(buf + n, sizeof buf - n, " [%u]null", i); continue; }
        n += snprintf(buf + n, sizeof buf - n, " [%u]t=%u f=0x%llx", i, L->type, (unsigned long long)L->layerFlags);
        if (L->type == 1000118003u) {   // XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB
            const XrCompositionLayerPassthroughFB* P = (const XrCompositionLayerPassthroughFB*)L;
            n += snprintf(buf + n, sizeof buf - n, " PT h=0x%llx", (unsigned long long)P->layerHandle);
            for (const XrBaseOutStructure* x = (const XrBaseOutStructure*)P->next; x && n < (int)sizeof buf - 20;
                 x = (const XrBaseOutStructure*)x->next)
                n += snprintf(buf + n, sizeof buf - n, " +%u", x->type);
        }
    }
    LOGI("%s", buf);
    return r;
}

// Log-only: passthrough state events (XR_TYPE_EVENT_DATA_PASSTHROUGH_STATE_CHANGED_FB).
typedef struct { uint32_t type; const void* next; uint64_t flags; } XrEventDataPassthroughStateChangedFB;
typedef XrResult (*PFN_poll_event)(XrInstance, XrBaseOutStructure*);
static PFN_poll_event g_orig_poll_event;
static XrResult hk_poll_event(XrInstance inst, XrBaseOutStructure* ev) {
    XrResult r = g_orig_poll_event(inst, ev);
    // Session state (XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED): hand joints
    // freeze while the session is not FOCUSED (s10-hmd-fix2), log to confirm.
    if (r == 0 && ev && ev->type == 18u) {
        typedef struct { uint32_t type; const void* next; uint64_t session; int32_t state; int64_t time; } XrEvSessionState;
        LOGI("session: state -> %d (4=VISIBLE 5=FOCUSED 6=STOPPING)", ((XrEvSessionState*)ev)->state);
    }
    if (r == 0 && ev && ev->type == 1000118030u)
        LOGI("pt: event PASSTHROUGH_STATE_CHANGED flags=0x%llx",
             (unsigned long long)((XrEventDataPassthroughStateChangedFB*)ev)->flags);
    return r;
}

// Log-only (§4.3c item 2): EyeGazeSpace = action space of VD's "eye_gaze_pose"
// action (XR.cs:748, Session.cs:968). xrLocateSpace results for it are logged
// ~2 Hz with the angle change since the previous logged sample.
static volatile XrAction g_gaze_action;
static volatile XrHandle g_gaze_space, g_gaze_session;
typedef struct { uint32_t type; const void* next; XrAction action; XrPath subactionPath; XrPosef poseInActionSpace; } XrActionSpaceCreateInfo;
typedef struct { uint32_t type; void* next; uint64_t locationFlags; XrPosef pose; } XrSpaceLocation;
typedef XrResult (*PFN_create_action_space)(XrSession, const XrActionSpaceCreateInfo*, XrHandle*);
typedef XrResult (*PFN_locate_space)(XrHandle, XrHandle, int64_t, XrSpaceLocation*);
static PFN_create_action_space g_orig_create_action_space;
static PFN_locate_space g_orig_locate_space;

static XrResult hk_create_action_space(XrSession s, const XrActionSpaceCreateInfo* ci, XrHandle* out) {
    XrResult r = g_orig_create_action_space(s, ci, out);
    if (r == 0 && ci && out && g_gaze_action && ci->action == g_gaze_action) {
        g_gaze_space = *out;
        g_gaze_session = s;
        LOGI("gaze: EyeGazeSpace=0x%llx session=0x%llx", (unsigned long long)*out, (unsigned long long)s);
    }
    return r;
}

// Eye gaze stabiliser (§4.3d, hand_gesture.txt gaze_filter, default 1).
// PICO's gaze_ext pose is the raw combined gaze (no smoothing in runtime or
// client) as head pose ∘ gaze. The gaze direction is taken relative to the head
// (VIEW space at the same time and base), One-Euro filtered there, and
// recomposed with the current head so head motion is not delayed. An invalid
// sample within gaze_hold_ms of the last valid one returns the held direction.
typedef struct { uint32_t type; const void* next; int32_t referenceSpaceType; XrPosef poseInReferenceSpace; } XrRefSpaceCI;
typedef XrResult (*PFN_create_ref_space)(XrSession, const XrRefSpaceCI*, XrHandle*);

static XrQuaternionf gq_mul(XrQuaternionf a, XrQuaternionf b) {
    XrQuaternionf r = { a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z };
    return r;
}
static XrQuaternionf gq_conj(XrQuaternionf q) { XrQuaternionf r = { -q.x, -q.y, -q.z, q.w }; return r; }
static XrVector3f gq_fwd(XrQuaternionf q) {   // q * (0,0,-1)
    XrVector3f v = { -2.0f * (q.x * q.z + q.w * q.y), -2.0f * (q.y * q.z - q.w * q.x), -(1.0f - 2.0f * (q.x * q.x + q.y * q.y)) };
    return v;
}
static float gv_dot(XrVector3f a, XrVector3f b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static XrVector3f gv_norm(XrVector3f a) {
    float l = sqrtf(gv_dot(a, a));
    XrVector3f r = { 0, 0, 0 };
    if (l > 1e-6f) { r.x = a.x / l; r.y = a.y / l; r.z = a.z / l; }
    return r;
}
static float gv_angle_deg(XrVector3f a, XrVector3f b) {
    float d = gv_dot(a, b);
    return acosf(d > 1 ? 1 : d < -1 ? -1 : d) * 57.29578f;
}
// Shortest-arc rotation from (0,0,-1) to unit d.
static XrQuaternionf gq_from_fwd(XrVector3f d) {
    // cross((0,0,-1), d) = (d.y, -d.x, 0); dot = -d.z
    XrQuaternionf q = { d.y, -d.x, 0.0f, 1.0f - d.z };
    float l = sqrtf(q.x * q.x + q.y * q.y + q.w * q.w);
    if (l < 1e-6f) { XrQuaternionf flip = { 0, 1, 0, 0 }; return flip; }   // d == (0,0,1)
    q.x /= l; q.y /= l; q.w /= l;
    return q;
}

// One-Euro filter on a 3-vector (Casiez et al. 2012).
typedef struct { int init; XrVector3f x, dx; } euro3;
static float euro_alpha(float cutoff, float dt) {
    float tau = 1.0f / (6.2831853f * cutoff);
    return 1.0f / (1.0f + tau / dt);
}
static XrVector3f euro3_step(euro3* f, XrVector3f v, float dt, float mincutoff, float beta, float dcutoff) {
    if (!f->init || dt <= 0) { f->init = 1; f->x = v; f->dx.x = f->dx.y = f->dx.z = 0; return v; }
    XrVector3f d = { (v.x - f->x.x) / dt, (v.y - f->x.y) / dt, (v.z - f->x.z) / dt };
    float ad = euro_alpha(dcutoff, dt);
    f->dx.x += ad * (d.x - f->dx.x); f->dx.y += ad * (d.y - f->dx.y); f->dx.z += ad * (d.z - f->dx.z);
    float speed = sqrtf(gv_dot(f->dx, f->dx));
    float a = euro_alpha(mincutoff + beta * speed, dt);
    f->x.x += a * (v.x - f->x.x); f->x.y += a * (v.y - f->x.y); f->x.z += a * (v.z - f->x.z);
    return f->x;
}

static struct {
    XrHandle view_session, view_space;
    PFN_create_ref_space create_ref_space;
    euro3 f;
    int64_t last_xr_time;         // last filtered sample time
    XrVector3f out_dir;           // filtered head-relative direction
    int64_t last_valid_ms;
    int have_out;
    XrVector3f pending;           // large jump awaiting confirmation
    int pending_n;
    unsigned held, rejected;      // counters for the log
} GZ;

static int gaze_head(XrHandle base, int64_t time, XrPosef* head) {
    XrHandle s = g_gaze_session;
    if (!s) return 0;
    if (!GZ.create_ref_space) {
        GZ.create_ref_space = (PFN_create_ref_space)vdhs_resolve_mapped_symbol(kLoaderNeedle, "xrCreateReferenceSpace");
        if (!GZ.create_ref_space) return 0;
    }
    if (GZ.view_session != s) {
        XrRefSpaceCI ci;
        memset(&ci, 0, sizeof ci);
        ci.type = 37;                     // XR_TYPE_REFERENCE_SPACE_CREATE_INFO
        ci.referenceSpaceType = 1;        // VIEW
        ci.poseInReferenceSpace.orientation.w = 1.0f;
        XrHandle sp = 0;
        XrResult r = GZ.create_ref_space(s, &ci, &sp);
        LOGI("gaze: VIEW space r=%d", r);
        GZ.view_session = s;
        GZ.view_space = r == 0 ? sp : 0;
    }
    if (!GZ.view_space) return 0;
    XrSpaceLocation hl;
    memset(&hl, 0, sizeof hl);
    hl.type = 42;                         // XR_TYPE_SPACE_LOCATION
    if (g_orig_locate_space(GZ.view_space, base, time, &hl) != 0 || (hl.locationFlags & 0x3) != 0x3) return 0;
    *head = hl.pose;
    return 1;
}

static void gaze_filter(XrHandle base, int64_t time, XrSpaceLocation* loc) {
    XrPosef head;
    if (!gaze_head(base, time, &head)) return;
    int64_t t = now_ms();
    int valid = (loc->locationFlags & 0x3) == 0x3;
    XrVector3f d = { 0, 0, 0 };
    if (valid) {
        XrQuaternionf q = loc->pose.orientation;
        float ql = sqrtf(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
        if (ql < 0.9f || ql > 1.1f) valid = 0;
        else d = gv_norm(gq_fwd(gq_mul(gq_conj(head.orientation), q)));   // head-relative
        if (gv_dot(d, d) < 0.5f) valid = 0;
    }
    if (valid && GZ.have_out && time != GZ.last_xr_time) {
        // First sample after a big jump (typical blink/lost-track artefact) is
        // held back until the next sample agrees with it.
        float jump = gv_angle_deg(d, GZ.out_dir);
        if (jump > vdhs_hand_cfg("gaze_jump_deg")) {
            if (GZ.pending_n == 0 || gv_angle_deg(d, GZ.pending) > 8.0f) {
                GZ.pending = d; GZ.pending_n = 1; GZ.rejected++;
                valid = 0;
            } else {
                GZ.pending_n = 0;
                GZ.f.init = 0;   // confirmed saccade: jump without smoothing lag
            }
        } else {
            GZ.pending_n = 0;
        }
    }
    if (valid) {
        if (time != GZ.last_xr_time) {
            float dt = GZ.last_xr_time ? (float)((time - GZ.last_xr_time) / 1e9) : 0;
            if (dt > 0.2f) GZ.f.init = 0;
            GZ.out_dir = gv_norm(euro3_step(&GZ.f, d, dt, vdhs_hand_cfg("gaze_mincutoff"),
                                             vdhs_hand_cfg("gaze_beta"), 1.0f));
            GZ.last_xr_time = time;
        }
        GZ.have_out = 1;
        GZ.last_valid_ms = t;
    } else if (GZ.have_out && t - GZ.last_valid_ms <= (int64_t)vdhs_hand_cfg("gaze_hold_ms")) {
        GZ.held++;
    } else {
        return;   // leave the runtime's (invalid) result
    }
    loc->pose.orientation = gq_mul(head.orientation, gq_from_fwd(GZ.out_dir));
    if (!(loc->locationFlags & 0x3)) loc->pose.position = head.position;
    loc->locationFlags |= 0xf;
}

// gaze_vd_fix (default 0, A/B only). On PICO VD computes
// Pose.Multiply(locate(Gaze,Stage), locate(Stage,Head)) = head-relative gaze
// (Session.cs:1446-1452) and uses it directly as a ray in its world
// (CurrentSpace = LOCAL, TouchInjector.cs:288-290), which drops the head pose.
// Returning X = head_in_stage ∘ T_local←stage ∘ gaze_stage makes VD's product
// equal the LOCAL-space gaze.
static XrPosef gpose_mul(XrPosef a, XrPosef b) {   // a ∘ b
    XrPosef r;
    r.orientation = gq_mul(a.orientation, b.orientation);
    XrQuaternionf p = { b.position.x, b.position.y, b.position.z, 0 };
    XrQuaternionf t = gq_mul(gq_mul(a.orientation, p), gq_conj(a.orientation));
    r.position.x = a.position.x + t.x; r.position.y = a.position.y + t.y; r.position.z = a.position.z + t.z;
    return r;
}
static XrHandle g_local_space, g_local_session;
static void gaze_vd_fix(XrHandle base, int64_t time, XrSpaceLocation* loc) {
    if ((loc->locationFlags & 0x3) != 0x3) return;
    // VD's world is Session.CurrentSpace (LOCAL, or STAGE with StageTracking),
    // taken from its projection layer. VD multiplies by head_from_stage in
    // either case, so the head term is needed even when world == base.
    XrHandle world = (XrHandle)vdhs_pt_current_space();
    XrPosef head;
    if (!gaze_head(base, time, &head)) return;
    XrHandle s = g_gaze_session;
    if (world == base) { loc->pose = gpose_mul(head, loc->pose); return; }
    if (world) {
        XrSpaceLocation sw;
        memset(&sw, 0, sizeof sw);
        sw.type = 42;
        if (g_orig_locate_space(base, world, time, &sw) != 0 || (sw.locationFlags & 0x3) != 0x3) return;
        loc->pose = gpose_mul(head, gpose_mul(sw.pose, loc->pose));
        return;
    }
    if (g_local_session != s) {
        XrRefSpaceCI ci;
        memset(&ci, 0, sizeof ci);
        ci.type = 37;
        ci.referenceSpaceType = 2;   // LOCAL
        ci.poseInReferenceSpace.orientation.w = 1.0f;
        XrHandle sp = 0;
        XrResult r = GZ.create_ref_space(s, &ci, &sp);
        LOGI("gaze: LOCAL space r=%d", r);
        g_local_session = s;
        g_local_space = r == 0 ? sp : 0;
    }
    if (!g_local_space) return;
    XrSpaceLocation sl;   // base (Stage) in LOCAL
    memset(&sl, 0, sizeof sl);
    sl.type = 42;
    if (g_orig_locate_space(base, g_local_space, time, &sl) != 0 || (sl.locationFlags & 0x3) != 0x3) return;
    loc->pose = gpose_mul(head, gpose_mul(sl.pose, loc->pose));
}

static void gaze_log_sample(XrHandle base, int64_t time, const XrSpaceLocation* loc, uint64_t raw_flags,
                            XrQuaternionf raw_q) {
    static int64_t last_ms;
    static unsigned calls, invalid;
    static XrQuaternionf last_raw, last_out;
    static int64_t last_time;
    calls++;
    if ((raw_flags & 0x3) != 0x3) invalid++;
    int64_t t = now_ms();
    if (t - last_ms < 500) return;
    const XrQuaternionf q = loc->pose.orientation;
    float d1 = fabsf(raw_q.x * last_raw.x + raw_q.y * last_raw.y + raw_q.z * last_raw.z + raw_q.w * last_raw.w);
    float d2 = fabsf(q.x * last_out.x + q.y * last_out.y + q.z * last_out.z + q.w * last_out.w);
    XrVector3f f = gq_fwd(q);
    // Upward-offset diagnosis (§4.3g): on PICO VD composes
    // locate(Gaze,Stage) with locate(Stage,Head) (Session.cs:1446-1452) and
    // casts the result as a world ray (TouchInjector.cs:288-290). Log the head
    // forward and the forward VD ends up with, so the offset can be compared
    // with head pitch offline. pitch = asin(fwd.y) in degrees.
    XrPosef head;
    float hp = NAN, gp = asinf(f.y > 1 ? 1 : f.y < -1 ? -1 : f.y) * 57.29578f, vp = NAN, rel = NAN;
    XrVector3f vf = { NAN, NAN, NAN };
    if (gaze_head(base, time, &head)) {
        XrVector3f hf = gq_fwd(head.orientation);
        hp = asinf(hf.y > 1 ? 1 : hf.y < -1 ? -1 : hf.y) * 57.29578f;
        XrVector3f rf = gq_fwd(gq_mul(gq_conj(head.orientation), q));   // gaze relative to head
        rel = asinf(rf.y > 1 ? 1 : rf.y < -1 ? -1 : rf.y) * 57.29578f;
        XrSpaceLocation sv;
        memset(&sv, 0, sizeof sv);
        sv.type = 42;
        if (g_orig_locate_space(base, GZ.view_space, time, &sv) == 0 && (sv.locationFlags & 0x1)) {
            vf = gq_fwd(gq_mul(sv.pose.orientation, q));   // Pose.Multiply(gaze, stageInHead)
            vp = asinf(vf.y > 1 ? 1 : vf.y < -1 ? -1 : vf.y) * 57.29578f;
        }
    }
    LOGI("gaze: locate base=0x%llx raw_flags=0x%llx out_flags=0x%llx p=(%.3f,%.3f,%.3f) fwd=(%.3f,%.3f,%.3f) "
         "dAng raw=%.1f out=%.1fdeg/%lldms dt_xr=%.1fms calls=%u invalid=%u held=%u rejected=%u "
         "pitch gaze=%.1f head=%.1f rel=%.1f vd=%.1f vd_fwd=(%.3f,%.3f,%.3f)",
         (unsigned long long)base, (unsigned long long)raw_flags, (unsigned long long)loc->locationFlags,
         loc->pose.position.x, loc->pose.position.y, loc->pose.position.z, f.x, f.y, f.z,
         2.0f * acosf(d1 > 1 ? 1 : d1) * 57.29578f, 2.0f * acosf(d2 > 1 ? 1 : d2) * 57.29578f,
         (long long)(t - last_ms), (time - last_time) / 1e6, calls, invalid, GZ.held, GZ.rejected,
         gp, hp, rel, vp, vf.x, vf.y, vf.z);
    last_ms = t; last_raw = raw_q; last_out = q; last_time = time; calls = invalid = 0;
    GZ.held = GZ.rejected = 0;
}

static XrResult hk_locate_space(XrHandle space, XrHandle base, int64_t time, XrSpaceLocation* loc) {
    XrResult r = g_orig_locate_space(space, base, time, loc);
    if (!g_gaze_space || space != g_gaze_space || !loc || r != 0) return r;
    static int enabled = -1, filter = -1, vdfix = -1;
    if (enabled < 0) {
        enabled = vdhs_hand_cfg("gaze_log") > 0;
        filter = vdhs_hand_cfg("gaze_filter") > 0;
        vdfix = vdhs_hand_cfg("gaze_vd_fix") > 0;
        LOGI("gaze: log=%d filter=%d vd_fix=%d", enabled, filter, vdfix);
    }
    const uint64_t raw_flags = loc->locationFlags;
    const XrQuaternionf raw_q = loc->pose.orientation;
    if (filter) gaze_filter(base, time, loc);
    if (enabled) gaze_log_sample(base, time, loc, raw_flags, raw_q);
    if (vdfix) gaze_vd_fix(base, time, loc);
    return r;
}

static XrResult hk_create_action(XrActionSet set, const XrActionCreateInfo* ci, XrAction* out) {
    XrResult r = g_orig_create_action(set, ci, out);
    if (r == 0 && ci && out && !strcmp(ci->actionName, "eye_gaze_pose")) {
        g_gaze_action = *out;
        LOGI("createAction: eye_gaze_pose=0x%llx", (unsigned long long)*out);
    }
    if (r == 0 && ci && out && !strcmp(ci->actionName, "grip_pose")) {
        pthread_mutex_lock(&g_lock);
        if (g_grip_n < MAX_GRIP) g_grip[g_grip_n++] = *out;
        pthread_mutex_unlock(&g_lock);
        LOGI("createAction: grip_pose=0x%llx", (unsigned long long)*out);
    }
    return r;
}

static int is_grip(XrAction a) {
    int n = g_grip_n;
    for (int i = 0; i < n; i++) if (g_grip[i] == a) return 1;
    return 0;
}

// PICO active input device, 0=HMD 1=controller 2=hand; cached ~20 ms.
static int active_input_cached(void) {
    static PFN_active_input fn;
    static int resolved;              // 0 = not tried, 1 = ok, -1 = refused
    static int64_t last_ms;
    static int last_val = -1;
    XrInstance inst = g_instance;
    if (!inst) return -10;
    int64_t t = now_ms();
    if (t - last_ms < 20) return last_val;
    last_ms = t;
    if (!resolved) {
        // Through VD's loader: it forwards to the runtime for enabled
        // extensions and stays valid across runtime reloads.
        XrResult r = g_orig_gipa(inst, "xrGetActiveInputDeviceTypePico", (PFN_xrVoidFunction*)&fn);
        resolved = (r == 0 && fn) ? 1 : -1;
        LOGI("activeInput: resolve r=%d fn=%p", r, (void*)fn);
    }
    if (resolved < 0) return last_val = -11;
    uint32_t v = 0xffffffffu;
    if (fn(inst, &v) != 0) return last_val = -12;
    if ((int)v != last_val) LOGI("activeInput: %d -> %u", last_val, v);
    return last_val = (int)v;
}

static XrResult hk_get_pose(XrSession s, const XrActionStateGetInfo* gi, XrActionStatePose* st) {
    XrResult r = g_orig_get_pose(s, gi, st);
    if (r == 0 && gi && st && st->isActive && is_grip(gi->action) && active_input_cached() == 2)
        st->isActive = 0;
    return r;
}

// ---------------------------------------------------------------- install
static int loader_mapped_cb(struct dl_phdr_info* info, size_t size, void* data) {
    const char* nm = info->dlpi_name;
    if (!nm) return 0;
    size_t ln = strlen(nm), lk = sizeof kLoaderNeedle - 1;
    if (ln >= lk && !strcmp(nm + ln - lk, kLoaderNeedle)) { *(uintptr_t*)data = info->dlpi_addr; return 1; }
    return 0;
}

typedef struct { const char* name; void* hook; void** orig; uintptr_t expect_off; } hook_spec;

static int install_hooks(uintptr_t base) {
    hook_spec specs[] = {
        { "xrEnumerateInstanceExtensionProperties", (void*)hk_enum,            (void**)&g_orig_enum,            0x385f4 },
        { "xrCreateInstance",                       (void*)hk_create_instance, (void**)&g_orig_create_instance, 0x39530 },
        { "xrGetInstanceProcAddr",                  (void*)hk_gipa,            (void**)&g_orig_gipa,            0 },
        { "xrCreateAction",                         (void*)hk_create_action,   (void**)&g_orig_create_action,   0x58cd4 },
        { "xrGetActionStatePose",                   (void*)hk_get_pose,        (void**)&g_orig_get_pose,        0x5a054 },
        { "xrGetSystemProperties",                  (void*)hk_get_system_props, (void**)&g_orig_get_system_props, 0x54058 },
        { "xrEndFrame",                             (void*)hk_end_frame,       (void**)&g_orig_end_frame,       0 },
        { "xrPollEvent",                            (void*)hk_poll_event,      (void**)&g_orig_poll_event,      0 },
        { "xrCreateActionSpace",                    (void*)hk_create_action_space, (void**)&g_orig_create_action_space, 0 },
        { "xrLocateSpace",                          (void*)hk_locate_space,    (void**)&g_orig_locate_space,    0 },
    };
    enum { N = (int)(sizeof specs / sizeof specs[0]) };
    void* addr[N];
    for (int i = 0; i < N; i++) {
        addr[i] = vdhs_resolve_mapped_symbol(kLoaderNeedle, specs[i].name);
        if (!addr[i]) { LOGE("install: %s not in .dynsym", specs[i].name); return -1; }
        uintptr_t off = (uintptr_t)addr[i] - base;
        LOGI("install: %s @ +0x%lx%s", specs[i].name, (unsigned long)off,
             specs[i].expect_off && off != specs[i].expect_off ? " (offset differs from 1.34.22)" : "");
    }
    int ok = 0;
    for (int i = 0; i < N; i++) {
        void* stub = shadowhook_hook_func_addr(addr[i], specs[i].hook, specs[i].orig);
        if (!stub) {
            int e = shadowhook_get_errno();
            LOGE("install: hook %s failed: %d %s", specs[i].name, e, shadowhook_to_errmsg(e));
            continue;
        }
        ok++;
    }
    vdhs_pt_install(base);   // swapchain hooks for pt_hole / pt_alpha_only (non-fatal)
    return ok == N ? 0 : -2;
}

static void* layer_thread(void* arg) {
    (void)arg;
    int rc = shadowhook_init(SHADOWHOOK_MODE_UNIQUE, false);
    LOGI("shadowhook %s init=%d", shadowhook_get_version(), rc);
    if (rc != 0) { LOGE("shadowhook init failed: %s", shadowhook_to_errmsg(rc)); return NULL; }

    // The loader is a regular app library (classloader namespace) that shows up
    // when VD's P/Invoke first binds it. dl_iterate_phdr walks every soinfo
    // regardless of namespace and is cheap enough to poll every 2 ms.
    uintptr_t base = 0;
    for (int i = 0; i < 60000 && !base; i++) {   // ~120 s
        dl_iterate_phdr(loader_mapped_cb, &base);
        if (!base) usleep(2000);
    }
    if (!base) { LOGE("libopenxr_loader.so never mapped"); return NULL; }
    LOGI("loader mapped at %p (+%lld ms)", (void*)base, TS());

    rc = install_hooks(base);
    if (rc == 0) g_hooks_ok = 1;
    LOGI("hooks %s (+%lld ms)", rc == 0 ? "installed" : "INCOMPLETE", TS());
    return NULL;
}

void vdhs_layer_start(void) {
    static int started;
    if (started) return;
    started = 1;
    g_t0 = now_ms();
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    int prc = pthread_create(&t, &a, layer_thread, NULL);
    pthread_attr_destroy(&a);
    LOGI("layer: start (spawn=%d)", prc);
}
