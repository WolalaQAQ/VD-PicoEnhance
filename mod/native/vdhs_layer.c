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
//                                                    or the controllers are idle (auto hand)
//   xrGetSystemProperties                  0x54058  log supportsHandTracking (VD's feature gate)
//   xrEndFrame / xrPollEvent               (sym)    layer-list / passthrough event logs
//   xrCreateActionSpace / xrLocateSpace    (sym)    auto hand switch: track grip-space
//                                                    motion (optional, hook kept if exported)
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

// Publish the hook group only after every required trampoline is available.
// Callbacks remain transparent during installation and after a failed rollback.
static int g_hooks_ok;
static int g_layer_started;
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

// Extensions the layer serves itself: advertised in enumerate,
// stripped before the runtime's xrCreateInstance. Off until step 3/4 lands so
// VD does not enable features the layer cannot serve yet.
static const char* const kLayerExts[] = { "XR_FB_hand_tracking_mesh", "XR_FB_hand_tracking_aim" };
enum { kLayerExtCount = (int)(sizeof kLayerExts / sizeof kLayerExts[0]) };
static const int g_advertise_layer_exts = 1;
static const char kHandExt[] = "XR_EXT_hand_tracking";
// PICO's own hand tracking ext: gates xrSetAppHandTrackingEnabledPico (runtime
// logs "Requires XR_PICO_hand_tracking extension enabled").
static const char kPicoHandExt[] = "XR_PICO_hand_tracking";
// Xenko.OpenXR.Extension values (from the app's managed assembly).
enum { kExtEnumHandTracking = 27, kExtEnumFbAim = 50, kExtEnumFbMesh = 52 };

// ---------------------------------------------------------------- passthrough diagnostics
// Log-only wrappers around VD's Hands passthrough path (PassthroughPortals.cs):
// return codes plus the geometry-instance pose/scale. Behaviour is unchanged.
typedef struct { float x, y, z, w; } XrQuaternionf;
typedef struct { float x, y, z; } XrVector3f;
typedef struct { XrQuaternionf orientation; XrVector3f position; } XrPosef;
typedef uint64_t XrHandle;

// ---------------------------------------------------------------- auto hand switch
// PICO keeps its input device on "controller" while a controller is merely set
// down, so VD never sees the "put it down" transition. Watch the grip action
// spaces' poses and, once they have been still for auto_hand_idle_ms, mask the
// grip pose so VD switches to hands by itself (see effective_hand_mode()).
typedef uint64_t XrSpace;
typedef int64_t XrTime;
typedef struct {
    uint32_t type; const void* next;
    XrAction action; XrPath subactionPath; XrPosef poseInActionSpace;
} XrActionSpaceCreateInfo;
typedef struct { uint32_t type; void* next; uint64_t locationFlags; XrPosef pose; } XrSpaceLocation;
#define XR_SPACE_LOCATION_POSITION_VALID_BIT 0x00000001ULL
typedef XrResult (*PFN_create_action_space)(XrSession, const XrActionSpaceCreateInfo*, XrSpace*);
typedef XrResult (*PFN_locate_space)(XrSpace, XrSpace, XrTime, XrSpaceLocation*);

static PFN_create_action_space g_orig_create_action_space;
static PFN_locate_space         g_orig_locate_space;
static XrSpace g_grip_space[MAX_GRIP];
static char g_grip_space_name[MAX_GRIP][64];   // action name bound to each tracked space
static volatile int g_grip_space_n;
static struct { XrSpace space; float x, y, z; int have; int move_logged; } g_ctrl[MAX_GRIP];
static volatile int64_t g_ctrl_last_move_ms;   // last tracked-space movement (> threshold)
static volatile int64_t g_ctrl_last_seen_ms;   // last tracked space located at all
static int g_auto_hand_logged;                  // transition log latch
static float g_auto_move_cm;                    // cached auto_hand_move_cm
static XrSpace g_ref_base;                       // base space captured from a VD locate
static volatile int64_t g_pred_time;             // last xrWaitFrame predictedDisplayTime

// Every xrCreateAction name, so the spaces VD binds can be identified in logs.
#define MAX_ACTIONS 64
static struct { XrAction action; char name[64]; } g_actions[MAX_ACTIONS];
static volatile int g_actions_n;
static const char* action_name(XrAction a) {
    int n = g_actions_n;
    for (int i = 0; i < n; i++) if (g_actions[i].action == a) return g_actions[i].name;
    return "?";
}

// VD resolves these two through xrGetInstanceProcAddr (not the loader export),
// so they are wrapped in g_gipa_table rather than inline-hooked.
static XrResult hk_create_action_space(XrSession, const XrActionSpaceCreateInfo*, XrSpace*);
static XrResult hk_locate_space(XrSpace, XrSpace, XrTime, XrSpaceLocation*);
static void ctrl_poll(void);
static void pico_app_hands_once(void);

// ---------------------------------------------------------------- controller input
// Once VD is showing hands it stops locating the aim_pose spaces, so a pick-up
// is invisible via motion. Buttons/triggers/axes are still polled through these
// three, so any (re)activation marks fresh controller activity.
typedef struct { uint32_t type; void* next; XrBool32 isActive; XrBool32 currentState; XrBool32 changed; int64_t lastChange; } XrActionStateBoolean;
typedef struct { uint32_t type; void* next; XrBool32 isActive; float currentState; XrBool32 changed; int64_t lastChange; } XrActionStateFloat;
typedef struct { uint32_t type; void* next; XrBool32 isActive; float x, y; XrBool32 changed; int64_t lastChange; } XrActionStateVector2f;
typedef XrResult (*PFN_get_bool)(XrSession, const XrActionStateGetInfo*, XrActionStateBoolean*);
typedef XrResult (*PFN_get_float)(XrSession, const XrActionStateGetInfo*, XrActionStateFloat*);
typedef XrResult (*PFN_get_vec2)(XrSession, const XrActionStateGetInfo*, XrActionStateVector2f*);
static PFN_get_bool  g_orig_get_bool;
static PFN_get_float g_orig_get_float;
static PFN_get_vec2  g_orig_get_vec2;
static XrResult hk_get_bool(XrSession, const XrActionStateGetInfo*, XrActionStateBoolean*);
static XrResult hk_get_float(XrSession, const XrActionStateGetInfo*, XrActionStateFloat*);
static XrResult hk_get_vec2(XrSession, const XrActionStateGetInfo*, XrActionStateVector2f*);

// xrWaitFrame: keep the latest predicted display time so we can poll the
// controller spaces ourselves while VD is in hand mode.
typedef struct { uint32_t type; void* next; XrTime predictedDisplayTime; XrTime predictedDisplayPeriod; XrBool32 shouldRender; } XrFrameState;
typedef XrResult (*PFN_wait_frame)(XrSession, XrFrameState*);
static PFN_wait_frame g_orig_wait_frame;
static XrResult hk_wait_frame(XrSession s, XrFrameState* st) {
    if (!g_orig_wait_frame) return (XrResult)-1;
    XrResult r = g_orig_wait_frame(s, st);
    if (r == 0 && st && st->predictedDisplayTime) g_pred_time = st->predictedDisplayTime;
    return r;
}

// Note fresh controller activity the moment any button/axis goes active.
static void ctrl_input_seen(const char* name) {
    int64_t t = now_ms();
    g_ctrl_last_move_ms = t;
    static int64_t last_log;
    if (t - last_log > 1000) { last_log = t; LOGI("input: controller button/axis active (%s)", name); }
}

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
    ROW("xrCreateActionSpace",              hk_create_action_space,        g_orig_create_action_space),
    ROW("xrLocateSpace",                    hk_locate_space,               g_orig_locate_space),
    ROW("xrGetActionStateBoolean",          hk_get_bool,                   g_orig_get_bool),
    ROW("xrGetActionStateFloat",            hk_get_float,                  g_orig_get_float),
    ROW("xrGetActionStateVector2f",         hk_get_vec2,                   g_orig_get_vec2),
    ROW("xrWaitFrame",                      hk_wait_frame,                 g_orig_wait_frame),
    { NULL, NULL, NULL },   // sentinel
};
#undef ROW

#define TS() ((long long)(now_ms() - g_t0))

int vdhs_layer_active(void) { return __atomic_load_n(&g_hooks_ok, __ATOMIC_ACQUIRE); }
int vdhs_layer_started(void) { return __atomic_load_n(&g_layer_started, __ATOMIC_ACQUIRE); }
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
    if (!vdhs_layer_active()) return g_orig_enum(layer, cap, count, props);
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

    // Append the extensions we make available to the runtime list. The loader
    // validates enabled names against this list, so XR_PICO_hand_tracking (which
    // gates xrSetAppHandTrackingEnabledPico) must appear here even though the
    // runtime itself does not advertise it.
    uint32_t n = 0;
    XrResult r = g_orig_enum(NULL, 0, &n, NULL);
    if (r != 0) return r;
    uint32_t total = n + kLayerExtCount + 1;   // +1 = XR_PICO_hand_tracking
    if (count) *count = total;
    if (cap == 0) return 0;
    if (cap < total) return -11;   // XR_ERROR_SIZE_INSUFFICIENT
    r = g_orig_enum(NULL, cap, &n, props);
    if (r != 0) return r;

    static int dumped;
    if (!dumped) {
        dumped = 1;
        for (uint32_t i = 0; i < n; i++) LOGI("enum[%u] = %s", i, props[i].extensionName);
    }

    const char* add[8];
    uint32_t na = 0;
    for (int i = 0; i < kLayerExtCount; i++) add[na++] = kLayerExts[i];
    add[na++] = kPicoHandExt;
    uint32_t w = n;
    for (uint32_t i = 0; i < na; i++) {
        int dup = 0;
        for (uint32_t j = 0; j < n; j++)
            if (!strcmp(props[j].extensionName, add[i])) { dup = 1; break; }
        if (dup) continue;
        memset(props[w].extensionName, 0, sizeof props[w].extensionName);
        strncpy(props[w].extensionName, add[i], sizeof props[w].extensionName - 1);
        props[w].extensionVersion = 1;
        w++;
    }
    if (count) *count = w;
    return 0;
}

static int is_layer_ext(const char* s) {
    for (int i = 0; i < kLayerExtCount; i++) if (!strcmp(s, kLayerExts[i])) return 1;
    return 0;
}

// Remove the first occurrence of `drop` from `names` in place; returns the new
// count. Used by the createInstance fallbacks.
static uint32_t drop_ext(const char** names, uint32_t m, const char* drop) {
    uint32_t w = 0;
    int removed = 0;
    for (uint32_t i = 0; i < m; i++) {
        if (!removed && !strcmp(names[i], drop)) { removed = 1; continue; }
        names[w++] = names[i];
    }
    return w;
}

static XrResult hk_create_instance(const XrInstanceCreateInfo* ci, XrInstance* out) {
    if (!vdhs_layer_active()) return g_orig_create_instance(ci, out);
    try_mark("createInstance");   // no-op when enumerate already marked
    if (!ci) return g_orig_create_instance(ci, out);

    uint32_t n = ci->enabledExtensionCount;
    // Neither the hand nor controller extension need be present in VD's list.
    const char** names = (const char**)calloc((size_t)n + 4, sizeof *names);
    if (!names) return g_orig_create_instance(ci, out);
    uint32_t m = 0;
    int has_ctrl = 0, stripped = 0, has_pico_hand = 0;
    for (uint32_t i = 0; i < n; i++) {
        const char* s = ci->enabledExtensionNames[i];
        if (!s) continue;
        if (is_layer_ext(s)) { stripped++; continue; }
        if (!strcmp(s, kControllerExt)) has_ctrl = 1;
        if (!strcmp(s, kPicoHandExt)) has_pico_hand = 1;
        names[m++] = s;
    }

    // Order matters for the fallbacks below:
    // [VD list][hand ext][pico hand ext][controller ext].
    int has_hand = 0;
    for (uint32_t i = 0; i < m; i++) if (!strcmp(names[i], kHandExt)) has_hand = 1;
    int hand_added = !has_hand;
    if (hand_added) names[m++] = kHandExt;
    int pico_hand_added = !has_pico_hand;
    if (pico_hand_added) names[m++] = kPicoHandExt;
    int added = !has_ctrl && g_marked;
    if (added) names[m++] = kControllerExt;

    XrInstanceCreateInfo copy = *ci;
    copy.enabledExtensionCount = m;
    copy.enabledExtensionNames = names;
    XrResult r = g_orig_create_instance(&copy, out);
    LOGI("createInstance: r=%d exts %u->%u (vd_had_ctrl=%d added=%d stripped=%d pico_hand=%d) (+%lld ms)",
         r, n, m, has_ctrl, added, stripped, has_pico_hand || pico_hand_added, TS());
    for (uint32_t i = 0; i < m; i++) LOGI("createInstance: ext[%u] = %s", i, names[i]);

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
    // failed xrCreateInstance (ReleasePassthrough NRE, observed). Drop the
    // extensions we appended, newest first.
    if (r == XR_ERROR_EXTENSION_NOT_PRESENT && added) {
        m = drop_ext(names, m, kControllerExt);
        copy.enabledExtensionCount = m;
        added = 0;
        r = g_orig_create_instance(&copy, out);
        LOGE("createInstance: controller ext rejected; without it r=%d", r);
    }
    if (r == XR_ERROR_EXTENSION_NOT_PRESENT && pico_hand_added) {
        m = drop_ext(names, m, kPicoHandExt);
        copy.enabledExtensionCount = m;
        pico_hand_added = 0;
        r = g_orig_create_instance(&copy, out);
        LOGE("createInstance: pico hand ext rejected; without it r=%d", r);
    }
    if (r == XR_ERROR_EXTENSION_NOT_PRESENT && hand_added) {
        m = drop_ext(names, m, kHandExt);
        copy.enabledExtensionCount = m;
        hand_added = 0;
        r = g_orig_create_instance(&copy, out);
        LOGE("createInstance: hand ext rejected; VD list only r=%d", r);
    }
    if (r == 0 && out) {
        // A fallback retry may have invalidated the cached runtime interface.
        // Refresh it on VD's thread before the managed input probe can run;
        // that background thread must never negotiate a runtime itself.
        vdhs_runtime_gipa();
        g_instance = *out;
    }
    LOGI("createInstance: final r=%d instance=0x%llx hand_ext=%d", r,
         (unsigned long long)(r == 0 && out ? *out : 0), has_hand || hand_added);
    free(names);

    // Plan A: VD has already stored the whitelist-filtered set in
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
    return r;
}

static XrResult hk_gipa(XrInstance inst, const char* name, PFN_xrVoidFunction* fn) {
    if (!vdhs_layer_active()) return g_orig_gipa(inst, name, fn);
    // Retry the runtime mark here too: by the time VD pulls its proc table the
    // forward loader is fully initialised, which the earlier attempts are not.
    try_mark("gipa");
    // Functions the layer implements itself: the runtime would answer
    // FUNCTION_UNSUPPORTED and VD's GetDelegate CheckError()s (XR.cs:641-649).
    if (inst && name && fn && vdhs_hand_gipa_self(name, fn)) return 0;
    XrResult r = g_orig_gipa(inst, name, fn);
    if (r != 0 || !name || !fn || !*fn) return r;
    if (vdhs_hand_gipa_wrap(name, fn)) return r;
    for (gipa_entry* e = g_gipa_table; e->name; e++) {
        if (strcmp(e->name, name) != 0) continue;
        // Keep an original already captured by an inline hook: overwriting it
        // with a pointer to the same hooked export would recurse.
        if (!*e->next) *e->next = *fn;
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
    if (!vdhs_layer_active()) return g_orig_get_system_props(inst, sys, props);
    XrResult r = g_orig_get_system_props(inst, sys, props);
    for (XrBaseOutStructure* s = props ? (XrBaseOutStructure*)props->next : NULL; s; s = (XrBaseOutStructure*)s->next) {
        if (s->type == 1000051000u)        // XR_TYPE_SYSTEM_HAND_TRACKING_PROPERTIES_EXT
            LOGI("systemProps: r=%d supportsHandTracking=%u", r, ((XrSystemHandTrackingPropertiesEXT*)s)->supportsHandTracking);
    }
    return r;
}

// Log-only: xrEndFrame layer list. Logged when the layer signature (count,
// blend mode, types, positions of passthrough layers) changes, plus ~every 10 s.
typedef XrResult (*PFN_end_frame)(XrSession, const XrFrameEndInfo*);
static PFN_end_frame g_orig_end_frame;

static XrResult hk_end_frame(XrSession s, const XrFrameEndInfo* fi_vd) {
    if (!vdhs_layer_active()) return g_orig_end_frame(s, fi_vd);
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
    ctrl_poll();
    pico_app_hands_once();
    XrResult r = g_orig_end_frame(s, fi);
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
    if (!vdhs_layer_active()) return g_orig_poll_event(inst, ev);
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

static XrResult hk_create_action(XrActionSet set, const XrActionCreateInfo* ci, XrAction* out) {
    if (!vdhs_layer_active()) return g_orig_create_action(set, ci, out);
    XrResult r = g_orig_create_action(set, ci, out);
    if (r == 0 && ci && out) {
        pthread_mutex_lock(&g_lock);
        if (g_actions_n < MAX_ACTIONS) {
            g_actions[g_actions_n].action = *out;
            strncpy(g_actions[g_actions_n].name, ci->actionName, sizeof g_actions[0].name - 1);
            g_actions[g_actions_n].name[sizeof g_actions[0].name - 1] = 0;
            g_actions_n++;
        }
        if (!strcmp(ci->actionName, "grip_pose") && g_grip_n < MAX_GRIP)
            g_grip[g_grip_n++] = *out;
        pthread_mutex_unlock(&g_lock);
        LOGI("createAction: '%s' type=%d -> 0x%llx", ci->actionName, (int)ci->actionType,
             (unsigned long long)*out);
    }
    return r;
}

static int is_grip(XrAction a) {
    int n = g_grip_n;
    for (int i = 0; i < n; i++) if (g_grip[i] == a) return 1;
    return 0;
}

// ---------------------------------------------------------------- auto hand switch
static int is_grip_space(XrSpace s) {
    int n = g_grip_space_n;
    for (int i = 0; i < n; i++) if (g_grip_space[i] == s) return 1;
    return 0;
}

static XrResult hk_create_action_space(XrSession s, const XrActionSpaceCreateInfo* ci, XrSpace* out) {
    if (!g_orig_create_action_space) return (XrResult)-1;
    XrResult r = g_orig_create_action_space(s, ci, out);
    const char* nm = ci ? action_name(ci->action) : "?";
    LOGI("createActionSpace: action=0x%llx name='%s' r=%d out=0x%llx grip=%d",
         (unsigned long long)(ci ? ci->action : 0), nm, r, (unsigned long long)(out ? *out : 0),
         ci ? is_grip(ci->action) : 0);
    if (r == 0 && ci && out) {
        pthread_mutex_lock(&g_lock);
        if (g_grip_space_n < MAX_GRIP) {
            int slot = g_grip_space_n++;
            g_grip_space[slot] = *out;
            strncpy(g_grip_space_name[slot], nm, sizeof g_grip_space_name[0] - 1);
            g_grip_space_name[slot][sizeof g_grip_space_name[0] - 1] = 0;
            LOGI("createActionSpace: tracking space '%s' 0x%llx as controller candidate",
                 g_grip_space_name[slot], (unsigned long long)*out);
        }
        pthread_mutex_unlock(&g_lock);
    }
    return r;
}

// Update the per-slot controller pose used by the auto-hand idle watcher.
static void ctrl_note_slot(int slot, const XrSpaceLocation* loc, int64_t t) {
    if (slot < 0 || !loc) return;
    g_ctrl_last_seen_ms = t;
    if (!(loc->locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) return;
    if (g_auto_move_cm <= 0) {
        g_auto_move_cm = vdhs_hand_cfg("auto_hand_move_cm");
        if (g_auto_move_cm <= 0) g_auto_move_cm = 2.0f;
    }
    float thr = g_auto_move_cm / 100.0f, thr2 = thr * thr;
    float dx = loc->pose.position.x - g_ctrl[slot].x;
    float dy = loc->pose.position.y - g_ctrl[slot].y;
    float dz = loc->pose.position.z - g_ctrl[slot].z;
    if (!g_ctrl[slot].have || dx*dx + dy*dy + dz*dz >= thr2) {
        if (g_ctrl[slot].have && !g_ctrl[slot].move_logged) {
            g_ctrl[slot].move_logged = 1;
            LOGI("locateSpace: space '%s' moves -> controller candidate", g_grip_space_name[slot]);
        }
        g_ctrl[slot].x = loc->pose.position.x;
        g_ctrl[slot].y = loc->pose.position.y;
        g_ctrl[slot].z = loc->pose.position.z;
        g_ctrl[slot].have = 1;
        g_ctrl_last_move_ms = t;
    }
}

static int ctrl_slot_of(XrSpace s) {
    int n = g_grip_space_n;
    for (int i = 0; i < n; i++) if (g_grip_space[i] == s) return i;
    return -1;
}

static XrResult hk_locate_space(XrSpace space, XrSpace base, XrTime time, XrSpaceLocation* loc) {
    if (!g_orig_locate_space) return (XrResult)-1;
    XrResult r = g_orig_locate_space(space, base, time, loc);
    if (r == 0 && loc && is_grip_space(space)) {
        static int logged;
        if (!logged) { logged = 1; LOGI("locateSpace: controller space located (auto-hand tracking active)"); }
        if (!g_ref_base && base) g_ref_base = base;   // remember a base for our own polls
        ctrl_note_slot(ctrl_slot_of(space), loc, now_ms());
    }
    if (time > g_pred_time) g_pred_time = time;       // fallback clock for our own polls
    return r;
}

// Per-frame poll of the controller action spaces. VD stops calling xrLocateSpace
// on them once it shows hands, so we locate them ourselves with the last frame's
// predicted time; this keeps the pick-up detection alive in hand mode.
static void ctrl_poll(void) {
    static int64_t last_log;
    int64_t t = now_ms();
    if (g_grip_space_n > 0 && g_ctrl_last_seen_ms == 0 && (t - last_log >= 5000)) {
        last_log = t;
        LOGI("ctrlPoll: waiting (base=%d time=%d n=%d orig=%d)",
             g_ref_base != 0, g_pred_time != 0, (int)g_grip_space_n, g_orig_locate_space != 0);
    }
    if (!g_orig_locate_space || !g_ref_base || g_pred_time == 0 || g_grip_space_n <= 0) return;
    if (t - last_log < 50) return;
    last_log = t;
    XrSpaceLocation loc;
    int valid = 0, rc0 = -9999;
    for (int i = 0; i < g_grip_space_n; i++) {
        memset(&loc, 0, sizeof loc);
        loc.type = 42;                 // XR_TYPE_SPACE_LOCATION
        loc.next = NULL;
        XrResult lr = g_orig_locate_space(g_grip_space[i], g_ref_base, (XrTime)g_pred_time, &loc);
        if (i == 0) rc0 = (int)lr;
        if (lr == 0)
            ctrl_note_slot(i, &loc, t);
        if (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) valid++;
    }
    static int64_t last_report; static int reported;
    if (!reported || t - last_report >= 5000) {
        reported = 1; last_report = t;
        LOGI("ctrlPoll: %d/%d spaces valid rc0=%d, moved %lldms ago",
             valid, (int)g_grip_space_n, rc0, (long long)(t - g_ctrl_last_move_ms));
    }
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
        // extensions and stays valid across runtime reloads. The exported name
        // differs across PICO OS builds (PICO 4 / 5.13.7 exposes the vendor
        // Pxr_ name, not the OpenXR-spelled one), so try the known spellings.
        static const char* const names[] = {
            "xrGetActiveInputDeviceTypePico",
            "Pxr_GetActiveInputDeviceType",
            "xrGetActiveInputTypePICO",
            "Pxr_GetHandTrackerActiveInputType",
        };
        XrResult r = -7;   // XR_ERROR_FUNCTION_UNSUPPORTED
        for (size_t i = 0; i < sizeof names / sizeof names[0] && !fn; i++) {
            PFN_active_input f = NULL;
            r = g_orig_gipa(inst, names[i], (PFN_xrVoidFunction*)&f);
            if (r == 0 && f) { fn = f; LOGI("activeInput: resolved via %s", names[i]); }
        }
        resolved = fn ? 1 : -1;
        LOGI("activeInput: resolve r=%d fn=%p%s", r, (void*)fn,
             fn ? "" : " (using pvr.active.input_device property)");
    }
    if (resolved < 0) {
        // No OpenXR active-input proc on this runtime: read the property the
        // PICO input reader maintains. Keep the last known value if it is
        // unavailable (e.g. SELinux denied the read).
        int v = vdhs_hand_active_input();
        if (v >= 0 && v != last_val) LOGI("activeInput: prop %d -> %d", last_val, v);
        return last_val = (v >= 0 ? v : last_val);
    }
    uint32_t v = 0xffffffffu;
    if (fn(inst, &v) != 0) return last_val;
    if ((int)v != last_val) LOGI("activeInput: %d -> %u", last_val, v);
    return last_val = (int)v;
}

// 1 = present the input as hands (mask the grip pose). PICO publishes its own
// decision as pvr.active.input_device (2 = hands, 1 = controller, other/-1 =
// undecided), but a resting controller keeps PICO on "controller" forever, so
// its value is only a hint. The real signal is our own xrLocateSpace poll of the
// controller (aim_pose) spaces: still for auto_hand_idle_ms means it was set
// down -> hands; any fresh movement means it was picked up -> controller.
static int effective_hand_mode(void) {
    int p = active_input_cached();
    static int prev_p = -2;
    int pico_hands = (p == 2);
    if (p == 1 && prev_p != 1) {                       // PICO just handed control back
        g_ctrl_last_move_ms = now_ms();                // treat as controller activity
        if (g_auto_hand_logged) { g_auto_hand_logged = 0; LOGI("autoHand: PICO -> controller"); }
    }
    prev_p = p;
    if (vdhs_hand_cfg("auto_hand") <= 0) return pico_hands;
    int64_t seen = g_ctrl_last_seen_ms;
    if (!seen) return pico_hands;                     // never observed a controller: follow PICO
    int64_t last = g_ctrl_last_move_ms ? g_ctrl_last_move_ms : seen;
    int64_t idle = now_ms() - last;
    float cfg = vdhs_hand_cfg("auto_hand_idle_ms");
    int64_t idle_ms = cfg > 0 ? (int64_t)cfg : 2000;
    if (idle < idle_ms) {
        if (g_auto_hand_logged) { g_auto_hand_logged = 0; LOGI("autoHand: controller moved -> controller (idle=%lldms)", (long long)idle); }
        return 0;
    }
    if (vdhs_hand_cfg("auto_hand_visible") > 0) {
        vdhs_pt_joint tmp[32];
        if (vdhs_pt_hand_snapshot(1, tmp, 32) <= 0 && vdhs_pt_hand_snapshot(2, tmp, 32) <= 0) return 0;
    }
    if (!g_auto_hand_logged) { g_auto_hand_logged = 1; LOGI("autoHand: controller idle %lldms -> hands", (long long)idle); }
    return 1;
}

static XrResult hk_get_bool(XrSession s, const XrActionStateGetInfo* gi, XrActionStateBoolean* st) {
    if (!vdhs_layer_active() || !g_orig_get_bool) return g_orig_get_bool ? g_orig_get_bool(s, gi, st) : (XrResult)-1;
    XrResult r = g_orig_get_bool(s, gi, st);
    if (r == 0 && gi && st && st->currentState) ctrl_input_seen(action_name(gi->action));
    return r;
}

static XrResult hk_get_float(XrSession s, const XrActionStateGetInfo* gi, XrActionStateFloat* st) {
    if (!vdhs_layer_active() || !g_orig_get_float) return g_orig_get_float ? g_orig_get_float(s, gi, st) : (XrResult)-1;
    XrResult r = g_orig_get_float(s, gi, st);
    if (r == 0 && gi && st && st->currentState > 0.4f) ctrl_input_seen(action_name(gi->action));
    return r;
}

static XrResult hk_get_vec2(XrSession s, const XrActionStateGetInfo* gi, XrActionStateVector2f* st) {
    if (!vdhs_layer_active() || !g_orig_get_vec2) return g_orig_get_vec2 ? g_orig_get_vec2(s, gi, st) : (XrResult)-1;
    XrResult r = g_orig_get_vec2(s, gi, st);
    if (r == 0 && gi && st && st->x * st->x + st->y * st->y > 0.36f) ctrl_input_seen(action_name(gi->action));
    return r;
}

static XrResult hk_get_pose(XrSession s, const XrActionStateGetInfo* gi, XrActionStatePose* st) {
    if (!vdhs_layer_active()) return g_orig_get_pose(s, gi, st);
    XrResult r = g_orig_get_pose(s, gi, st);
    if (r == 0 && gi && st && st->isActive && is_grip(gi->action) && effective_hand_mode())
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

// Best-effort hook: a missing export or a failed hook only disables that
// feature, it never rolls the core group back.
static void install_optional_hook(uintptr_t base, const char* name, void* hook, void** orig) {
    void* a = vdhs_resolve_mapped_symbol(kLoaderNeedle, name);
    if (!a) { LOGE("install: %s not in .dynsym (auto-hand switch disabled)", name); return; }
    void* stub = shadowhook_hook_func_addr(a, hook, orig);
    if (!stub) {
        int e = shadowhook_get_errno();
        LOGE("install: hook %s failed: %d %s (auto-hand switch disabled)", name, e, shadowhook_to_errmsg(e));
        *orig = NULL;
        return;
    }
    LOGI("install: %s @ +0x%lx (auto-hand)", name, (unsigned long)((uintptr_t)a - base));
}

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
    void* stubs[N] = { 0 };
    for (int i = 0; i < N; i++) {
        stubs[i] = shadowhook_hook_func_addr(addr[i], specs[i].hook, specs[i].orig);
        if (!stubs[i]) {
            int e = shadowhook_get_errno();
            LOGE("install: hook %s failed: %d %s", specs[i].name, e, shadowhook_to_errmsg(e));
            for (int j = i - 1; j >= 0; j--) {
                int urc = shadowhook_unhook(stubs[j]);
                if (urc != 0) LOGE("install: rollback %s failed: %d", specs[j].name, urc);
            }
            // Keep orig slots intact for callbacks already in flight (or a hook
            // that could not be removed). The group stays disabled in both cases.
            return -2;
        }
    }
    install_optional_hook(base, "xrCreateActionSpace", (void*)hk_create_action_space, (void**)&g_orig_create_action_space);
    install_optional_hook(base, "xrLocateSpace", (void*)hk_locate_space, (void**)&g_orig_locate_space);
    install_optional_hook(base, "xrGetActionStateBoolean", (void*)hk_get_bool, (void**)&g_orig_get_bool);
    install_optional_hook(base, "xrGetActionStateFloat", (void*)hk_get_float, (void**)&g_orig_get_float);
    install_optional_hook(base, "xrGetActionStateVector2f", (void*)hk_get_vec2, (void**)&g_orig_get_vec2);
    install_optional_hook(base, "xrWaitFrame", (void*)hk_wait_frame, (void**)&g_orig_wait_frame);
    if (vdhs_pt_install(base) != 0) LOGE("install: alpha-hole hooks disabled");
    __atomic_store_n(&g_hooks_ok, 1, __ATOMIC_RELEASE);
    return 0;
}

// PICO's OpenXR hand-tracking SDK calls xrSetAppHandTrackingEnabledPico(true) so
// the system knows the app owns hand input and turns off its hand-gesture
// shortcuts (recenter / open dock). VD uses generic OpenXR and never calls it,
// so those gestures keep firing. We call it on VD's behalf. Non-fatal: any
// failure just leaves the system gestures on.
typedef XrResult (*PFN_set_app_hands)(XrInstance, uint32_t enabled);
static PFN_set_app_hands g_pfn_set_app_hands;

static void pico_app_hands_once(void) {
    static int done;
    static int tries;
    static int64_t last;
    if (done) return;
    if (vdhs_hand_cfg("pico_app_hands") <= 0) { done = 1; return; }
    XrInstance inst = g_instance;
    if (!inst || !g_orig_gipa) return;
    int64_t t = now_ms();
    if (t - last < 500) return;
    last = t;
    if (!g_pfn_set_app_hands) {
        PFN_xrVoidFunction f = NULL;
        // Prefer the runtime's own proc table (obtained via negotiate): VD's
        // loader rejects PICO extensions we have not been able to mark, while
        // the runtime gates on the API class mark instead.
        PFN_gipa rg = (PFN_gipa)vdhs_runtime_gipa();
        XrResult rr = rg ? rg(inst, "xrSetAppHandTrackingEnabledPico", &f) : -1;
        if ((rr != 0 || !f) && g_orig_gipa)
            rr = g_orig_gipa(inst, "xrSetAppHandTrackingEnabledPico", &f);
        if (rr != 0 || !f) {
            if (++tries >= 6) { done = 1; LOGI("picoAppHands: xrSetAppHandTrackingEnabledPico r=%d (unavailable)", rr); }
            return;
        }
        g_pfn_set_app_hands = (PFN_set_app_hands)f;
        LOGI("picoAppHands: resolved xrSetAppHandTrackingEnabledPico");
    }
    XrResult rc = g_pfn_set_app_hands(inst, 1);
    if (tries == 0 || rc == 0) LOGI("picoAppHands: xrSetAppHandTrackingEnabledPico(1) -> %d", rc);
    if (rc == 0 || ++tries > 20) {
        if (rc != 0) LOGI("picoAppHands: giving up (rc=%d)", rc);
        done = 1;
    }
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
    LOGI("hooks %s (+%lld ms)", rc == 0 ? "installed" : "DISABLED", TS());
    return NULL;
}

void vdhs_layer_start(void) {
    if (__atomic_exchange_n(&g_layer_started, 1, __ATOMIC_ACQ_REL)) return;
    g_t0 = now_ms();
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    int prc = pthread_create(&t, &a, layer_thread, NULL);
    pthread_attr_destroy(&a);
    LOGI("layer: start (spawn=%d)", prc);
}
