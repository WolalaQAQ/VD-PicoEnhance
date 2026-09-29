// VD compat layer: zero-copy alpha hole in the VR projection swapchain
// (docs/HAND-VD-COMPAT-LAYER-2026-09-28.md §4.3e).
//
// These four functions are NOT reached through xrGetInstanceProcAddr: VD's
// managed side P/Invokes the loader exports directly (DllImport
// "libopenxr_loader"), so they get the same ShadowHook treatment as the other
// loader exports in vdhs_layer.c.
//
// Only the user-approved "zero copy + scissor" approach is implemented:
//   - no swapchain is created and no image is copied;
//   - inside xrReleaseSwapchainImage, before the image goes back to the
//     compositor, the alpha channel of VD's own VR texture is set to 0 inside
//     one scissor-bounded disc per hand (the 0.1 m window VD uses on Quest:
//     a flat disc at hand joint 12 facing the head, PassthroughPortals.cs);
//   - RGB is masked out and nothing is cleared, so no full-screen write and no
//     more GPU bandwidth than the disc area;
//   - no glFinish / glReadPixels; GPU cost is read back asynchronously through
//     GL_EXT_disjoint_timer_query.
//
// Hand joints come from vdhs_hand.c's latest valid locate snapshot (same space
// as the projection layer). Frozen or stale data is not drawn.
//
// Disabled unless hand_gesture.txt sets pt_hole=1 (draw) or pt_alpha_only=1
// (OR 0x6 into the projection layer flags only, to probe whether VD's alpha is
// 1 outside the hands).
#define _GNU_SOURCE
#include <dlfcn.h>
#include <math.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <android/log.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>

#include "shadowhook.h"
#include "vdhs_layer.h"

#define TAG "vdhs-layer"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

void* vdhs_resolve_mapped_symbol(const char* lib, const char* name);   // vdhs_payload.c

// ---------------------------------------------------------------- OpenXR subset
typedef int32_t XrResult;
typedef uint64_t XrSwapchain, XrSession, XrSpace;
typedef int64_t XrTime;
typedef void (*PFN_xrVoidFunction)(void);

typedef struct { float x, y, z, w; } XrQuaternionf;
typedef struct { float x, y, z; } XrVector3f;
typedef struct { XrQuaternionf orientation; XrVector3f position; } XrPosef;
typedef struct { float angleLeft, angleRight, angleUp, angleDown; } XrFovf;
typedef struct { int32_t x, y; } XrOffset2Di;
typedef struct { int32_t width, height; } XrExtent2Di;
typedef struct { XrOffset2Di offset; XrExtent2Di extent; } XrRect2Di;

typedef struct { XrSwapchain swapchain; XrRect2Di imageRect; uint32_t imageArrayIndex; } XrSwapchainSubImage;
typedef struct {
    uint32_t type; const void* next; XrPosef pose; XrFovf fov; XrSwapchainSubImage subImage;
} XrCompositionLayerProjectionView;
typedef struct {
    uint32_t type; const void* next; uint64_t layerFlags; XrSpace space;
    uint32_t viewCount; const XrCompositionLayerProjectionView* views;
} XrCompositionLayerProjection;
typedef struct { uint32_t type; const void* next; uint64_t layerFlags; } XrCompositionLayerBaseHeader;
typedef struct {
    uint32_t type; const void* next; XrTime displayTime; int32_t environmentBlendMode;
    uint32_t layerCount; const XrCompositionLayerBaseHeader* const* layers;
} XrFrameEndInfo;
typedef struct {
    uint32_t type; const void* next; int64_t createFlags; uint64_t usageFlags; int64_t format;
    uint32_t sampleCount, width, height, faceCount, arraySize, mipCount;
} XrSwapchainCreateInfo;
typedef struct { uint32_t type; void* next; uint32_t image; } XrSwapchainImageOpenGLKHR;
typedef struct { uint32_t type; const void* next; } XrSwapchainImageInfo;   // acquire / release

// Layout guards for the hand-rolled structs above (same ABI as the OpenXR
// headers; the wrapped signatures must accept exactly what VD passes).
_Static_assert(sizeof(XrFrameEndInfo) == 40, "XrFrameEndInfo");
_Static_assert(offsetof(XrFrameEndInfo, layers) == 32, "XrFrameEndInfo.layers");
_Static_assert(sizeof(XrCompositionLayerProjection) == 48, "XrCompositionLayerProjection");
_Static_assert(offsetof(XrCompositionLayerProjection, views) == 40, "projection views");
_Static_assert(offsetof(XrCompositionLayerProjectionView, fov) == 44, "view fov");
_Static_assert(offsetof(XrCompositionLayerProjectionView, subImage) == 64, "view subImage");
_Static_assert(sizeof(XrCompositionLayerProjectionView) == 96, "XrCompositionLayerProjectionView");
_Static_assert(sizeof(XrSwapchainSubImage) == 32, "XrSwapchainSubImage");
_Static_assert(sizeof(XrSwapchainCreateInfo) == 64, "XrSwapchainCreateInfo");
_Static_assert(offsetof(XrSwapchainCreateInfo, width) == 44, "create width");
_Static_assert(sizeof(XrSwapchainImageOpenGLKHR) == 24, "XrSwapchainImageOpenGLKHR");

#define XR_TYPE_COMPOSITION_LAYER_PROJECTION 35u
#define XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB 1000118003u
#define XR_LAYER_FLAG_BLEND_ALPHA  0x2ull
#define XR_LAYER_FLAG_UNPREMULT    0x4ull
#define XR_LOCATION_POSITION_VALID 0x2ull

typedef XrResult (*PFN_create_swapchain)(XrSession, const XrSwapchainCreateInfo*, XrSwapchain*);
typedef XrResult (*PFN_enum_swapchain_images)(XrSwapchain, uint32_t, uint32_t*, XrSwapchainImageOpenGLKHR*);
typedef XrResult (*PFN_acquire_swapchain_image)(XrSwapchain, const XrSwapchainImageInfo*, uint32_t*);
typedef XrResult (*PFN_release_swapchain_image)(XrSwapchain, const XrSwapchainImageInfo*);

static PFN_create_swapchain         g_orig_create_swapchain;
static PFN_enum_swapchain_images    g_orig_enum_swapchain_images;
static PFN_acquire_swapchain_image  g_orig_acquire_swapchain_image;
static PFN_release_swapchain_image  g_orig_release_swapchain_image;
typedef XrResult (*PFN_destroy_swapchain)(XrSwapchain);
static PFN_destroy_swapchain        g_orig_destroy_swapchain;

// Space of VD's projection layers = Session.CurrentSpace (OpenXRHMD.cs:988),
// the world VD casts its gaze ray in. Read by vdhs_layer.c's gaze_vd_fix.
static volatile XrSpace g_current_space;
uint64_t vdhs_pt_current_space(void) { return g_current_space; }

static const char kLoaderNeedle[] = "/lib/arm64/libopenxr_loader.so";

// ---------------------------------------------------------------- config gate
static int pt_cfg_state;   // 0 untried, 1 = on, -1 = off
static int pt_enabled(void) {
    if (!pt_cfg_state) {
        int on = vdhs_hand_cfg("pt_hole") > 0 || vdhs_hand_cfg("pt_alpha_only") > 0;
        pt_cfg_state = on ? 1 : -1;
        LOGI("pt: alpha hole %s (pt_hole=%.0f pt_alpha_only=%.0f)", on ? "enabled" : "disabled",
             vdhs_hand_cfg("pt_hole"), vdhs_hand_cfg("pt_alpha_only"));
    }
    return pt_cfg_state > 0;
}

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
static int64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

// ---------------------------------------------------------------- swapchain table
#define PT_MAX_SC 32
#define PT_MAX_TEX 8
typedef struct {
    XrSwapchain h;
    int64_t format;
    uint32_t width, height, arraySize;
    int is_vr;
    uint32_t tex_n;
    uint32_t tex[PT_MAX_TEX];
    uint32_t acquired;      // UINT32_MAX = none
} pt_sc;
static pt_sc g_sc[PT_MAX_SC];
static pthread_mutex_t g_sc_lock = PTHREAD_MUTEX_INITIALIZER;

static pt_sc* sc_find_locked(XrSwapchain h) {
    for (int i = 0; i < PT_MAX_SC; i++) if (g_sc[i].h == h) return &g_sc[i];
    return NULL;
}
static void sc_remember_image(XrSwapchain h, uint32_t index, uint32_t image) {
    pthread_mutex_lock(&g_sc_lock);
    pt_sc* s = sc_find_locked(h);
    if (s && index < PT_MAX_TEX) {
        s->tex[index] = image;
        if (index + 1 > s->tex_n) s->tex_n = index + 1;
    }
    pthread_mutex_unlock(&g_sc_lock);
}

// ---------------------------------------------------------------- VR views
// Recorded from the endFrame projection layer. The view pose/fov memory is
// allocated once by VD and rewritten by BeginDraw*VR before rendering, so the
// pointer is enough and the release hook reads the current frame's values.
static const XrCompositionLayerProjectionView* g_vr_views;
static uint32_t g_vr_view_count;
static XrSwapchain g_vr_chain[4];
static int g_vr_n;
static pthread_mutex_t g_vr_lock = PTHREAD_MUTEX_INITIALIZER;

// Forget the recorded views: nothing is drawn until the next endFrame that
// passes the gate records them again.
static void vr_views_clear(void) {
    pthread_mutex_lock(&g_vr_lock);
    g_vr_views = NULL;
    g_vr_view_count = 0;
    g_vr_n = 0;
    pthread_mutex_unlock(&g_vr_lock);
}

static int chain_in(XrSwapchain h, const uint64_t* chains, int n) {
    for (int i = 0; i < n; i++) if (chains[i] == h) return 1;
    return 0;
}

void vdhs_pt_on_end_frame(const void* frame_end_info) {
    const XrFrameEndInfo* fi = (const XrFrameEndInfo*)frame_end_info;
    for (uint32_t i = 0; fi && fi->layers && i < fi->layerCount; i++)
        if (fi->layers[i] && fi->layers[i]->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
            g_current_space = ((const XrCompositionLayerProjection*)fi->layers[i])->space;
            break;
        }
    if (!pt_enabled()) return;
    // pt_follow_settings (default 1): only when VD itself would show hand
    // passthrough in SteamVR, and only on the layer that carries VD's VR
    // stream swapchains. Otherwise the frame is left untouched.
    int follow = vdhs_hand_cfg("pt_follow_settings") > 0;
    uint64_t chains[8];
    int nchains = 0;
    if (!fi || !fi->layers || !fi->layerCount) { if (follow) vr_views_clear(); return; }
    // Per-frame part of the gate: the managed settings are only re-read every
    // ~1 s, but VD adds its projected passthrough layer (hand portals,
    // OpenXRHMD.DrawPassthrough) in the same frame it shows them. No hole is
    // cut in frames without one, and its appearance triggers a fast re-read
    // (s10-hmd-steamvr: the gate lagged the portals by 0.65 s).
    if (follow) {
        int has_pt = 0;
        for (uint32_t i = 0; i < fi->layerCount && !has_pt; i++)
            has_pt = fi->layers[i] && fi->layers[i]->type == XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB;
        if (!vdhs_hand_pt_gate(chains, 8, &nchains, has_pt) || !has_pt) { vr_views_clear(); return; }
    }
    int found = 0;
    for (uint32_t i = 0; i < fi->layerCount; i++) {
        const XrCompositionLayerBaseHeader* L = fi->layers[i];
        if (!L || L->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION) continue;
        const XrCompositionLayerProjection* P = (const XrCompositionLayerProjection*)L;
        if (!P->views || !P->viewCount) continue;
        if (follow && !chain_in(P->views[0].subImage.swapchain, chains, nchains)) continue;
        // The layer gets 0x6 (blend source alpha + unpremultiplied) so PICO
        // composites it against whatever is below (chroma key uses 0x7 and
        // works on PICO 5.x).
        ((XrCompositionLayerBaseHeader*)L)->layerFlags |= XR_LAYER_FLAG_BLEND_ALPHA | XR_LAYER_FLAG_UNPREMULT;
        pthread_mutex_lock(&g_vr_lock);
        g_vr_views = P->views;
        g_vr_view_count = P->viewCount;
        g_vr_n = (int)(P->viewCount > 4 ? 4 : P->viewCount);
        for (int v = 0; v < g_vr_n; v++) g_vr_chain[v] = P->views[v].subImage.swapchain;
        pthread_mutex_unlock(&g_vr_lock);
        pthread_mutex_lock(&g_sc_lock);
        for (int v = 0; v < g_vr_n; v++) {
            pt_sc* s = sc_find_locked(g_vr_chain[v]);
            if (s) s->is_vr = 1;
            else {
                static XrSwapchain warned;
                if (warned != g_vr_chain[v]) {
                    warned = g_vr_chain[v];
                    LOGE("pt: view %d swapchain 0x%llx not tracked (no hole in this eye)", v,
                         (unsigned long long)g_vr_chain[v]);
                }
            }
        }
        pthread_mutex_unlock(&g_sc_lock);
        found = 1;
        if (follow) break;   // exactly one VR stream layer
    }
    if (!found) { vr_views_clear(); return; }
    static XrSwapchain logged_chain;
    if (logged_chain != g_vr_chain[0]) {
        logged_chain = g_vr_chain[0];
        LOGI("pt: projection layer found handles=%d band=%s follow=%d chain0=0x%llx", g_vr_n,
             vdhs_hand_cfg("pt_hole") > 0 ? "hole" : "alpha_only", follow, (unsigned long long)logged_chain);
    }
}

// ---------------------------------------------------------------- GL entry points
typedef void* (*PFN_eglGetProcAddress)(const char*);
typedef void* (*PFN_eglGetCurrentContext)(void);

static void* g_egl_handle;
static PFN_eglGetProcAddress g_egl_gpa;
static PFN_eglGetCurrentContext g_egl_current;

static void* glsym(const char* n) {
    void* p = g_egl_gpa ? g_egl_gpa(n) : NULL;
    if (!p) p = dlsym(RTLD_DEFAULT, n);
    return p;
}

static struct {
    PFNGLVIEWPORTPROC Viewport;
    PFNGLSCISSORPROC Scissor;
    PFNGLCOLORMASKPROC ColorMask;
    PFNGLENABLEPROC Enable;
    PFNGLDISABLEPROC Disable;
    PFNGLISENABLEDPROC IsEnabled;
    PFNGLGETINTEGERVPROC GetIntegerv;
    PFNGLGETBOOLEANVPROC GetBooleanv;
    PFNGLBINDFRAMEBUFFERPROC BindFramebuffer;
    PFNGLGENFRAMEBUFFERSPROC GenFramebuffers;
    PFNGLDELETEFRAMEBUFFERSPROC DeleteFramebuffers;
    PFNGLFRAMEBUFFERTEXTURE2DPROC FramebufferTexture2D;
    PFNGLCHECKFRAMEBUFFERSTATUSPROC CheckFramebufferStatus;
    PFNGLCREATESHADERPROC CreateShader;
    PFNGLSHADERSOURCEPROC ShaderSource;
    PFNGLCOMPILESHADERPROC CompileShader;
    PFNGLGETSHADERIVPROC GetShaderiv;
    PFNGLGETSHADERINFOLOGPROC GetShaderInfoLog;
    PFNGLDELETESHADERPROC DeleteShader;
    PFNGLCREATEPROGRAMPROC CreateProgram;
    PFNGLATTACHSHADERPROC AttachShader;
    PFNGLLINKPROGRAMPROC LinkProgram;
    PFNGLGETPROGRAMIVPROC GetProgramiv;
    PFNGLGETPROGRAMINFOLOGPROC GetProgramInfoLog;
    PFNGLUSEPROGRAMPROC UseProgram;
    PFNGLDELETEPROGRAMPROC DeleteProgram;
    PFNGLGENBUFFERSPROC GenBuffers;
    PFNGLDELETEBUFFERSPROC DeleteBuffers;
    PFNGLBINDBUFFERPROC BindBuffer;
    PFNGLBUFFERDATAPROC BufferData;
    PFNGLGENVERTEXARRAYSPROC GenVertexArrays;
    PFNGLDELETEVERTEXARRAYSPROC DeleteVertexArrays;
    PFNGLBINDVERTEXARRAYPROC BindVertexArray;
    PFNGLENABLEVERTEXATTRIBARRAYPROC EnableVertexAttribArray;
    PFNGLVERTEXATTRIBPOINTERPROC VertexAttribPointer;
    PFNGLDRAWARRAYSPROC DrawArrays;
    PFNGLGETERRORPROC GetError;
    PFNGLFLUSHPROC Flush;
} GLF;

typedef struct { PFNGLGENQUERIESEXTPROC GenQueries;
                 PFNGLDELETEQUERIESEXTPROC DeleteQueries;
                 PFNGLBEGINQUERYEXTPROC BeginQuery;
                 PFNGLENDQUERYEXTPROC EndQuery;
                 PFNGLGETQUERYOBJECTUIVEXTPROC GetQueryUiv;
                 PFNGLGETQUERYOBJECTUI64VEXTPROC GetQueryUi64; } pt_query_api;
static pt_query_api QA;

// Timer query ring: one measurement in flight per slot, results read back only
// when the driver reports them available (never blocks).
static struct { GLuint q; int pending; } g_q[4];

static int g_gl_state;   // 0 untried, 1 ok, -1 failed
static void* g_gl_ctx;   // EGLContext the objects were created in
static GLuint g_fbo, g_prog, g_vao, g_vbo;
static int g_fbo_incomplete_logged;

static GLuint pt_compile(GLenum type, const char* src) {
    GLuint s = GLF.CreateShader(type);
    if (!s) return 0;
    GLF.ShaderSource(s, 1, &src, NULL);
    GLF.CompileShader(s);
    GLint ok = 0;
    GLF.GetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[256] = "";
        GLF.GetShaderInfoLog(s, sizeof log, NULL, log);
        LOGE("pt: shader %u compile failed: %s", type, log);
        GLF.DeleteShader(s);
        return 0;
    }
    return s;
}

// Lazily resolve EGL/GL and build the FBO + program. Must run with a current
// GL context (the first xrReleaseSwapchainImage of a VR chain).
static int pt_egl_init(void) {
    if (g_egl_handle) return (g_egl_gpa && g_egl_current) ? 1 : -1;
    g_egl_handle = dlopen("libEGL.so", RTLD_LAZY | RTLD_LOCAL);
    if (!g_egl_handle) { LOGE("pt: dlopen libEGL.so failed: %s", dlerror()); return -1; }
    g_egl_gpa = (PFN_eglGetProcAddress)dlsym(g_egl_handle, "eglGetProcAddress");
    g_egl_current = (PFN_eglGetCurrentContext)dlsym(g_egl_handle, "eglGetCurrentContext");
    if (!g_egl_gpa || !g_egl_current) { LOGE("pt: libEGL entry points missing"); return -1; }
    return 1;
}

static int pt_gl_init(void) {
    if (pt_egl_init() < 0) { g_gl_state = -1; return 0; }
    void* cur = g_egl_current();      // NULL when no context is current yet
    if (!cur) return 0;               // retry later
    if (g_gl_state == 1) {
        if (g_gl_ctx == cur) return 1;
        g_gl_state = 0;               // context changed: rebuild the objects
    }
    if (g_gl_state == -1) return 0;
    g_gl_state = -1;

    static const struct { const char* n; void** p; } req[] = {
        { "glViewport", (void**)&GLF.Viewport }, { "glScissor", (void**)&GLF.Scissor },
        { "glColorMask", (void**)&GLF.ColorMask }, { "glEnable", (void**)&GLF.Enable },
        { "glDisable", (void**)&GLF.Disable }, { "glIsEnabled", (void**)&GLF.IsEnabled },
        { "glGetIntegerv", (void**)&GLF.GetIntegerv }, { "glGetBooleanv", (void**)&GLF.GetBooleanv },
        { "glBindFramebuffer", (void**)&GLF.BindFramebuffer }, { "glGenFramebuffers", (void**)&GLF.GenFramebuffers },
        { "glDeleteFramebuffers", (void**)&GLF.DeleteFramebuffers },
        { "glFramebufferTexture2D", (void**)&GLF.FramebufferTexture2D },
        { "glCheckFramebufferStatus", (void**)&GLF.CheckFramebufferStatus },
        { "glCreateShader", (void**)&GLF.CreateShader }, { "glShaderSource", (void**)&GLF.ShaderSource },
        { "glCompileShader", (void**)&GLF.CompileShader }, { "glGetShaderiv", (void**)&GLF.GetShaderiv },
        { "glGetShaderInfoLog", (void**)&GLF.GetShaderInfoLog }, { "glDeleteShader", (void**)&GLF.DeleteShader },
        { "glCreateProgram", (void**)&GLF.CreateProgram }, { "glAttachShader", (void**)&GLF.AttachShader },
        { "glLinkProgram", (void**)&GLF.LinkProgram }, { "glGetProgramiv", (void**)&GLF.GetProgramiv },
        { "glGetProgramInfoLog", (void**)&GLF.GetProgramInfoLog }, { "glUseProgram", (void**)&GLF.UseProgram },
        { "glDeleteProgram", (void**)&GLF.DeleteProgram },
        { "glGenBuffers", (void**)&GLF.GenBuffers }, { "glDeleteBuffers", (void**)&GLF.DeleteBuffers },
        { "glBindBuffer", (void**)&GLF.BindBuffer }, { "glBufferData", (void**)&GLF.BufferData },
        { "glGenVertexArrays", (void**)&GLF.GenVertexArrays }, { "glDeleteVertexArrays", (void**)&GLF.DeleteVertexArrays },
        { "glBindVertexArray", (void**)&GLF.BindVertexArray },
        { "glEnableVertexAttribArray", (void**)&GLF.EnableVertexAttribArray },
        { "glVertexAttribPointer", (void**)&GLF.VertexAttribPointer }, { "glDrawArrays", (void**)&GLF.DrawArrays },
        { "glGetError", (void**)&GLF.GetError }, { "glFlush", (void**)&GLF.Flush },
    };
    for (size_t i = 0; i < sizeof req / sizeof req[0]; i++) {
        *req[i].p = glsym(req[i].n);
        if (!*req[i].p) { LOGE("pt: GL entry %s missing", req[i].n); return 0; }
    }
    QA.GenQueries = (PFNGLGENQUERIESEXTPROC)glsym("glGenQueriesEXT");
    QA.DeleteQueries = (PFNGLDELETEQUERIESEXTPROC)glsym("glDeleteQueriesEXT");
    QA.BeginQuery = (PFNGLBEGINQUERYEXTPROC)glsym("glBeginQueryEXT");
    QA.EndQuery = (PFNGLENDQUERYEXTPROC)glsym("glEndQueryEXT");
    QA.GetQueryUiv = (PFNGLGETQUERYOBJECTUIVEXTPROC)glsym("glGetQueryObjectuivEXT");
    QA.GetQueryUi64 = (PFNGLGETQUERYOBJECTUI64VEXTPROC)glsym("glGetQueryObjectui64vEXT");
    if (!QA.GenQueries || !QA.BeginQuery || !QA.EndQuery || !QA.GetQueryUiv)
        LOGE("pt: timer query unavailable; GPU time will be skipped");

    static const char* vs = "#version 300 es\nlayout(location=0) in vec2 a;\nvoid main(){gl_Position=vec4(a,0.0,1.0);}\n";
    static const char* fs = "#version 300 es\nprecision mediump float;\nout vec4 o;\nvoid main(){o=vec4(0.0);}\n";
    GLuint v = pt_compile(GL_VERTEX_SHADER, vs), f = pt_compile(GL_FRAGMENT_SHADER, fs);
    if (!v || !f) { if (v) GLF.DeleteShader(v); if (f) GLF.DeleteShader(f); return 0; }
    g_prog = GLF.CreateProgram();
    GLF.AttachShader(g_prog, v);
    GLF.AttachShader(g_prog, f);
    GLF.LinkProgram(g_prog);
    GLF.DeleteShader(v);
    GLF.DeleteShader(f);
    GLint ok = 0;
    GLF.GetProgramiv(g_prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[256] = "";
        GLF.GetProgramInfoLog(g_prog, sizeof log, NULL, log);
        LOGE("pt: program link failed: %s", log);
        return 0;
    }
    GLF.GenFramebuffers(1, &g_fbo);
    GLint pv = 0, pb = 0;
    GLF.GetIntegerv(GL_VERTEX_ARRAY_BINDING, &pv);
    GLF.GetIntegerv(GL_ARRAY_BUFFER_BINDING, &pb);
    GLF.GenVertexArrays(1, &g_vao);
    GLF.GenBuffers(1, &g_vbo);
    GLF.BindVertexArray(g_vao);
    GLF.BindBuffer(GL_ARRAY_BUFFER, g_vbo);
    GLF.EnableVertexAttribArray(0);
    GLF.VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, (const void*)0);
    GLF.BindVertexArray((GLuint)pv);
    GLF.BindBuffer(GL_ARRAY_BUFFER, (GLuint)pb);
    if (QA.GenQueries) {
        for (int i = 0; i < 4; i++) { g_q[i].q = 0; QA.GenQueries(1, &g_q[i].q); }
    }
    g_gl_ctx = cur;
    g_gl_state = 1;
    LOGI("pt: GL ready program=%u fbo=%u vao=%u query=%s", g_prog, g_fbo, g_vao, QA.GenQueries ? "yes" : "no");
    return 1;
}

// ---------------------------------------------------------------- stats
#define PT_STAT_MAX 512
static double g_stat_gpu[PT_STAT_MAX], g_stat_cpu[PT_STAT_MAX], g_stat_area[PT_STAT_MAX];
static unsigned g_stat_gpu_n, g_stat_cpu_n;
static int64_t g_stat_last_ms;

static double pt_p95(const double* v, unsigned n) {
    if (!n) return 0;
    double c[PT_STAT_MAX];
    memcpy(c, v, n * sizeof c[0]);
    for (unsigned i = 1; i < n; i++) {   // insertion sort, n <= 512
        double x = c[i];
        unsigned j = i;
        while (j > 0 && c[j - 1] > x) { c[j] = c[j - 1]; j--; }
        c[j] = x;
    }
    unsigned k = (unsigned)((n - 1) * 0.95);
    return c[k];
}
static void pt_stat_add(double gpu_us, double cpu_us, double area) {
    if (gpu_us >= 0 && g_stat_gpu_n < PT_STAT_MAX) g_stat_gpu[g_stat_gpu_n++] = gpu_us;
    if (cpu_us >= 0 && g_stat_cpu_n < PT_STAT_MAX) {
        g_stat_cpu[g_stat_cpu_n] = cpu_us;
        g_stat_area[g_stat_cpu_n] = area;
        g_stat_cpu_n++;
    }
}
static void pt_stat_flush(void) {
    int64_t t = now_ms();
    if (t - g_stat_last_ms < 2000) return;
    if (!g_stat_last_ms) { g_stat_last_ms = t; return; }
    g_stat_last_ms = t;
    if (!g_stat_cpu_n && !g_stat_gpu_n) return;
    double gsum = 0, gmax = 0, csum = 0, cmax = 0, asum = 0, amax = 0;
    for (unsigned i = 0; i < g_stat_gpu_n; i++) { gsum += g_stat_gpu[i]; if (g_stat_gpu[i] > gmax) gmax = g_stat_gpu[i]; }
    for (unsigned i = 0; i < g_stat_cpu_n; i++) {
        csum += g_stat_cpu[i]; if (g_stat_cpu[i] > cmax) cmax = g_stat_cpu[i];
        asum += g_stat_area[i]; if (g_stat_area[i] > amax) amax = g_stat_area[i];
    }
    LOGI("pt: hole gpu n=%u avg=%.1fus p95=%.1fus max=%.1fus | cpu n=%u avg=%.1fus p95=%.1fus max=%.1fus | "
         "scissor avg=%.2f%% max=%.2f%%",
         g_stat_gpu_n, g_stat_gpu_n ? gsum / g_stat_gpu_n : 0.0, pt_p95(g_stat_gpu, g_stat_gpu_n), gmax,
         g_stat_cpu_n, g_stat_cpu_n ? csum / g_stat_cpu_n : 0.0, pt_p95(g_stat_cpu, g_stat_cpu_n), cmax,
         g_stat_cpu_n ? 100.0 * asum / g_stat_cpu_n : 0.0, 100.0 * amax);
    g_stat_gpu_n = g_stat_cpu_n = 0;
}

// Timer query ring: one measurement in flight per slot, results read back only
// when the driver reports them available (never blocks).
static int g_q_next;

static int pt_query_begin(void) {
    if (!QA.GenQueries || !QA.BeginQuery || !QA.EndQuery) return 0;
    for (int i = 0; i < 4; i++) {
        int s = (g_q_next + i) % 4;
        if (g_q[s].pending || !g_q[s].q) continue;
        g_q_next = (s + 1) % 4;
        QA.BeginQuery(GL_TIME_ELAPSED_EXT, g_q[s].q);
        return s + 1;   // slot + 1 (0 = none)
    }
    return 0;
}
static void pt_query_poll(void) {
    if (!QA.GetQueryUiv) return;
    // GL_GPU_DISJOINT_EXT is cleared by reading it, so sample it once per pass
    // and apply the same answer to every query read back this round.
    GLint disjoint = 0;
    GLF.GetIntegerv(GL_GPU_DISJOINT_EXT, &disjoint);
    for (int i = 0; i < 4; i++) {
        if (!g_q[i].pending) continue;
        GLuint avail = 0;
        QA.GetQueryUiv(g_q[i].q, GL_QUERY_RESULT_AVAILABLE_EXT, &avail);
        if (!avail) continue;
        if (!disjoint) {
            if (QA.GetQueryUi64) {
                GLuint64 ns = 0;
                QA.GetQueryUi64(g_q[i].q, GL_QUERY_RESULT_EXT, &ns);
                pt_stat_add((double)ns / 1000.0, -1, 0);
            } else {
                GLuint ns = 0;
                QA.GetQueryUiv(g_q[i].q, GL_QUERY_RESULT_EXT, &ns);
                pt_stat_add((double)ns / 1000.0, -1, 0);
            }
        }
        g_q[i].pending = 0;
    }
}
static void pt_query_end(int slot1) {
    if (slot1 > 0) { QA.EndQuery(GL_TIME_ELAPSED_EXT); g_q[slot1 - 1].pending = 1; }
}

// ---------------------------------------------------------------- projection
static XrVector3f vsub3(XrVector3f a, XrVector3f b) { XrVector3f r = { a.x - b.x, a.y - b.y, a.z - b.z }; return r; }
static XrVector3f qrot3(XrQuaternionf q, XrVector3f v) {
    XrVector3f u = { q.x, q.y, q.z };
    XrVector3f t = { 2.0f * (u.y * v.z - u.z * v.y), 2.0f * (u.z * v.x - u.x * v.z), 2.0f * (u.x * v.y - u.y * v.x) };
    XrVector3f r = { v.x + q.w * t.x + (u.y * t.z - u.z * t.y),
                     v.y + q.w * t.y + (u.z * t.x - u.x * t.z),
                     v.z + q.w * t.z + (u.x * t.y - u.y * t.x) };
    return r;
}

typedef struct { float x0, y0, x1, y1; } pt_rect;

// The VD/Quest hand passthrough window is a flat disc of radius 0.1 m centred
// on hand joint 12 (MIDDLE_PROXIMAL) and oriented to face the head
// (PassthroughPortals.cs:135-140,301). Modelling the hole as a SPHERE of the
// same radius at that joint makes the silhouette a circle from any viewing
// angle; using one pixel radius keeps it a circle on screen even with an
// asymmetric fov. This emits one triangle fan plus its pixel bounding box.
static int pt_emit_disc(const XrPosef* vp, float tl, float tr, float tu, float td, int flip_y,
                        const vdhs_pt_joint* j, int n, float radius,
                        uint32_t vx, uint32_t vy, uint32_t vw, uint32_t vh,
                        float* verts, int max_floats, pt_rect* bb) {
    const int SEG = 24;
    const int J_MIDDLE_PROX = 12;
    if (n <= J_MIDDLE_PROX) return 0;
    const vdhs_pt_joint* c = &j[J_MIDDLE_PROX];
    if (!(c->flags & XR_LOCATION_POSITION_VALID) || !(radius > 0.0f)) return 0;
    XrQuaternionf qc = { -vp->orientation.x, -vp->orientation.y, -vp->orientation.z, vp->orientation.w };
    XrVector3f p = { c->x, c->y, c->z };
    XrVector3f v = qrot3(qc, vsub3(p, vp->position));
    float depth = -v.z;
    if (!(depth > 0.05f)) return 0;
    float d2 = v.x * v.x + v.y * v.y + v.z * v.z;
    if (!(d2 > 1e-4f)) return 0;
    float R = radius;
    if (R * R >= d2) R = sqrtf(d2) * 0.99f;          // keep the sphere in front
    float tant = R / sqrtf(d2 - R * R);              // silhouette: tan(asin(R/d))
    float nx = (2.0f * v.x / depth - (tr + tl)) / (tr - tl);
    float ny = (2.0f * v.y / depth - (tu + td)) / (tu - td);
    // One pixel radius -> a screen circle regardless of orientation / fov.
    float fpx = 0.5f * ((float)vw / (tr - tl) + (float)vh / (tu - td));
    float rpx = tant * fpx;
    if (!(rpx > 0.5f) || !isfinite(rpx)) return 0;
    float rx = 2.0f * rpx / (float)vw;
    float ry = 2.0f * rpx / (float)vh;
    if (flip_y) ny = -ny;
    float sx = (float)vx + (nx * 0.5f + 0.5f) * (float)vw;
    float sy = (float)vy + (ny * 0.5f + 0.5f) * (float)vh;
    bb->x0 = sx - rpx; bb->y0 = sy - rpx; bb->x1 = sx + rpx; bb->y1 = sy + rpx;
    int nf = 0;
    for (int k = 0; k < SEG && nf + 6 <= max_floats; k++) {
        float a0 = (float)k * 6.2831853f / (float)SEG, a1 = (float)(k + 1) * 6.2831853f / (float)SEG;
        verts[nf++] = nx; verts[nf++] = ny;
        verts[nf++] = nx + rx * cosf(a0); verts[nf++] = ny + ry * sinf(a0);
        verts[nf++] = nx + rx * cosf(a1); verts[nf++] = ny + ry * sinf(a1);
    }
    return nf;
}

// ---------------------------------------------------------------- release hook
static void pt_do_holes(XrSwapchain sc) {
    if (vdhs_hand_cfg("pt_hole") <= 0) return;

    pthread_mutex_lock(&g_vr_lock);
    const XrCompositionLayerProjectionView* views = g_vr_views;
    int vn = g_vr_n;
    XrSwapchain chains[4];
    for (int i = 0; i < 4; i++) chains[i] = g_vr_chain[i];
    pthread_mutex_unlock(&g_vr_lock);
    if (!views || vn <= 0) return;

    const XrCompositionLayerProjectionView* v = NULL;
    for (int i = 0; i < vn; i++) if (chains[i] == sc) { v = &views[i]; break; }
    if (!v) return;

    pt_sc* s;
    uint32_t tex = 0, w = 0, h = 0;
    pthread_mutex_lock(&g_sc_lock);
    s = sc_find_locked(sc);
    if (s && s->is_vr && s->acquired != UINT32_MAX && s->acquired < s->tex_n) {
        tex = s->tex[s->acquired];
        w = s->width; h = s->height;
    }
    pthread_mutex_unlock(&g_sc_lock);
    if (!tex || !w || !h) return;

    // Only from here on are we guaranteed to be on the render thread with the
    // VR swapchain's GL context current, which is what the GL query reads need.
    if (!pt_gl_init()) return;

    pt_query_poll();
    pt_stat_flush();

    vdhs_pt_joint jl[26], jr[26];
    int nl = vdhs_pt_hand_snapshot(1, jl, 26);
    int nr = vdhs_pt_hand_snapshot(2, jr, 26);
    if (!nl && !nr) return;

    float tl = tanf(v->fov.angleLeft), tr = tanf(v->fov.angleRight);
    float tu = tanf(v->fov.angleUp), td = tanf(v->fov.angleDown);
    if (!(tr - tl > 1e-4f) || !(tu - td > 1e-4f)) return;

    uint32_t vx = (uint32_t)(v->subImage.imageRect.offset.x < 0 ? 0 : v->subImage.imageRect.offset.x);
    uint32_t vy = (uint32_t)(v->subImage.imageRect.offset.y < 0 ? 0 : v->subImage.imageRect.offset.y);
    if (vx >= w || vy >= h) return;
    uint32_t vw = (uint32_t)(v->subImage.imageRect.extent.width > 0 ? v->subImage.imageRect.extent.width : (int32_t)w);
    uint32_t vh = (uint32_t)(v->subImage.imageRect.extent.height > 0 ? v->subImage.imageRect.extent.height : (int32_t)h);
    if (vw > w) vw = w;
    if (vh > h) vh = h;
    if (vx + vw > w) vw = w - vx;
    if (vy + vh > h) vh = h - vy;
    if (!vw || !vh) return;

    int flip_y = vdhs_hand_cfg("pt_flip_y") > 0;
    float margin = vdhs_hand_cfg("pt_margin_px");
    float disc_r = vdhs_hand_cfg("pt_disc_r");

    int64_t cpu0 = now_us();

    // Save every piece of state this function touches.
    GLint prev_draw_fbo = 0, prev_read_fbo = 0, prev_vp[4], prev_sc[4], prev_prog = 0, prev_vao = 0, prev_abo = 0;
    GLboolean prev_cm[4], prev_scissor, prev_blend, prev_depth, prev_cull;
    GLF.GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev_draw_fbo);
    GLF.GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev_read_fbo);
    GLF.GetIntegerv(GL_VIEWPORT, prev_vp);
    GLF.GetIntegerv(GL_SCISSOR_BOX, prev_sc);
    GLF.GetIntegerv(GL_CURRENT_PROGRAM, &prev_prog);
    GLF.GetIntegerv(GL_VERTEX_ARRAY_BINDING, &prev_vao);
    GLF.GetIntegerv(GL_ARRAY_BUFFER_BINDING, &prev_abo);
    GLF.GetBooleanv(GL_COLOR_WRITEMASK, prev_cm);
    prev_scissor = GLF.IsEnabled(GL_SCISSOR_TEST);
    prev_blend = GLF.IsEnabled(GL_BLEND);
    prev_depth = GLF.IsEnabled(GL_DEPTH_TEST);
    prev_cull = GLF.IsEnabled(GL_CULL_FACE);

    GLF.BindFramebuffer(GL_DRAW_FRAMEBUFFER, g_fbo);
    GLF.FramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    GLenum st = GLF.CheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE) {
        if (!g_fbo_incomplete_logged) { g_fbo_incomplete_logged = 1; LOGE("pt: FBO incomplete 0x%x tex=%u", st, tex); }
        GLF.FramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
        GLF.BindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)prev_draw_fbo);
        GLF.BindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)prev_read_fbo);
        return;
    }

    GLF.Viewport((GLint)vx, (GLint)vy, (GLsizei)vw, (GLsizei)vh);
    GLF.UseProgram(g_prog);
    GLF.ColorMask(0, 0, 0, 1);
    GLF.Disable(GL_BLEND);
    GLF.Disable(GL_DEPTH_TEST);
    GLF.Disable(GL_CULL_FACE);
    GLF.Enable(GL_SCISSOR_TEST);
    GLF.BindVertexArray(g_vao);
    GLF.BindBuffer(GL_ARRAY_BUFFER, g_vbo);

    int q1 = pt_query_begin();

    float verts[26 * 30 * 2];
    double area = 0;
    int hands = 0;
    for (int pass = 0; pass < 2; pass++) {
        const vdhs_pt_joint* j = pass ? jr : jl;
        int n = pass ? nr : nl;
        if (!n) continue;
        pt_rect bb;
        int nf = pt_emit_disc(&v->pose, tl, tr, tu, td, flip_y, j, n, disc_r, vx, vy, vw, vh,
                              verts, (int)(sizeof verts / sizeof verts[0]), &bb);
        if (nf < 6) continue;
        float x0 = bb.x0 - margin, y0 = bb.y0 - margin, x1 = bb.x1 + margin, y1 = bb.y1 + margin;
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > (float)w) x1 = (float)w;
        if (y1 > (float)h) y1 = (float)h;
        int sx = (int)x0, sy = (int)y0, sw = (int)(x1 - x0), sh = (int)(y1 - y0);
        if (sw <= 0 || sh <= 0) continue;
        GLF.Scissor(sx, sy, sw, sh);
        GLF.BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(nf * (int)sizeof(float)), verts, GL_DYNAMIC_DRAW);
        GLF.DrawArrays(GL_TRIANGLES, 0, nf / 2);
        area += (double)sw * sh / ((double)w * h);
        hands++;
    }

    pt_query_end(q1);
    GLF.Flush();

    // Restore exactly what was saved; the CPU cost includes the restore.
    GLF.BindVertexArray((GLuint)prev_vao);
    GLF.BindBuffer(GL_ARRAY_BUFFER, (GLuint)prev_abo);
    GLF.UseProgram((GLuint)prev_prog);
    GLF.ColorMask(prev_cm[0], prev_cm[1], prev_cm[2], prev_cm[3]);
    if (prev_blend) GLF.Enable(GL_BLEND); else GLF.Disable(GL_BLEND);
    if (prev_depth) GLF.Enable(GL_DEPTH_TEST); else GLF.Disable(GL_DEPTH_TEST);
    if (prev_cull) GLF.Enable(GL_CULL_FACE); else GLF.Disable(GL_CULL_FACE);
    if (prev_scissor) GLF.Enable(GL_SCISSOR_TEST); else GLF.Disable(GL_SCISSOR_TEST);
    GLF.Scissor(prev_sc[0], prev_sc[1], prev_sc[2], prev_sc[3]);
    GLF.Viewport(prev_vp[0], prev_vp[1], prev_vp[2], prev_vp[3]);
    GLF.FramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);   // no stale texture
    GLF.BindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)prev_draw_fbo);
    GLF.BindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)prev_read_fbo);

    // Do not leave a sticky error behind for VD's own glGetError (bounded: a
    // lost context must not spin this render thread).
    for (int i = 0; i < 16; i++) if (GLF.GetError() == GL_NO_ERROR) break;

    int64_t cpu1 = now_us();
    if (hands) pt_stat_add(-1, (double)(cpu1 - cpu0), area);
    static int logged;
    if (hands && !logged) {
        logged = 1;
        LOGI("pt: first hole draw v=%p views=%p hands=%d scissor=%.2f%% cpu=%lldus", (void*)v, (void*)views, hands,
             area * 100.0, (long long)(cpu1 - cpu0));
    }
}

static XrResult hk_create_swapchain(XrSession s, const XrSwapchainCreateInfo* ci, XrSwapchain* out) {
    XrResult r = g_orig_create_swapchain(s, ci, out);
    if (r == 0 && out && ci && pt_enabled()) {
        // VD recreates its VR chains after ReleaseVR freed the old views
        // array (OpenXRHMD.cs:3026-3038), and handles get reused
        // (s9-hmd-pt-big: 0x746f6457c0 at 1970 then 2688 px). Drop the
        // recorded views so release never reads the freed array.
        vr_views_clear();
        pthread_mutex_lock(&g_sc_lock);
        pt_sc* slot = NULL;
        for (int i = 0; i < PT_MAX_SC; i++) {
            if (g_sc[i].h == *out) { slot = &g_sc[i]; break; }
            if (!g_sc[i].h && !slot) slot = &g_sc[i];
        }
        // Table full (a destroy was missed): recycle the oldest entry rather
        // than leave this chain untracked (s10: 19 handles, left eye lost).
        if (!slot) {
            static int next_evict;
            slot = &g_sc[next_evict];
            next_evict = (next_evict + 1) % PT_MAX_SC;
            LOGE("pt: swapchain table full, recycling slot h=0x%llx", (unsigned long long)slot->h);
        }
        if (slot) {
            memset(slot, 0, sizeof *slot);
            slot->h = *out;
            slot->format = ci->format;
            slot->width = ci->width;
            slot->height = ci->height;
            slot->arraySize = ci->arraySize;
            slot->acquired = UINT32_MAX;
        }
        pthread_mutex_unlock(&g_sc_lock);
        LOGI("pt: createSwapchain h=0x%llx fmt=%lld %ux%u arr=%u r=%d", (unsigned long long)*out,
             (long long)ci->format, ci->width, ci->height, ci->arraySize, r);
    }
    return r;
}

static XrResult hk_enum_swapchain_images(XrSwapchain sc, uint32_t cap, uint32_t* count,
                                         XrSwapchainImageOpenGLKHR* images) {
    XrResult r = g_orig_enum_swapchain_images(sc, cap, count, images);
    if (r == 0 && images && cap && count && pt_enabled()) {
        uint32_t n = *count < cap ? *count : cap;
        for (uint32_t i = 0; i < n; i++) sc_remember_image(sc, i, images[i].image);
    }
    return r;
}

static XrResult hk_acquire_swapchain_image(XrSwapchain sc, const XrSwapchainImageInfo* info, uint32_t* index) {
    XrResult r = g_orig_acquire_swapchain_image(sc, info, index);
    if (r == 0 && index && pt_enabled()) {
        pthread_mutex_lock(&g_sc_lock);
        pt_sc* s = sc_find_locked(sc);
        if (s) s->acquired = *index;
        pthread_mutex_unlock(&g_sc_lock);
    }
    return r;
}

static XrResult hk_release_swapchain_image(XrSwapchain sc, const XrSwapchainImageInfo* info) {
    if (pt_enabled()) pt_do_holes(sc);
    return g_orig_release_swapchain_image(sc, info);
}

// Free the table slot; VD disposes its chains on every ReleaseVR
// (OpenXRHMD.cs:3009-3020) and SteamVR reconnects create new ones.
static XrResult hk_destroy_swapchain(XrSwapchain sc) {
    if (pt_enabled()) {
        pthread_mutex_lock(&g_sc_lock);
        pt_sc* s = sc_find_locked(sc);
        if (s) memset(s, 0, sizeof *s);
        pthread_mutex_unlock(&g_sc_lock);
        vr_views_clear();
    }
    return g_orig_destroy_swapchain(sc);
}

// ---------------------------------------------------------------- install
int vdhs_pt_install(uintptr_t base) {
    static const struct { const char* name; void* hook; void** orig; } specs[] = {
        { "xrCreateSwapchain",          (void*)hk_create_swapchain,         (void**)&g_orig_create_swapchain },
        { "xrEnumerateSwapchainImages", (void*)hk_enum_swapchain_images,    (void**)&g_orig_enum_swapchain_images },
        { "xrAcquireSwapchainImage",    (void*)hk_acquire_swapchain_image,  (void**)&g_orig_acquire_swapchain_image },
        { "xrReleaseSwapchainImage",    (void*)hk_release_swapchain_image,  (void**)&g_orig_release_swapchain_image },
        { "xrDestroySwapchain",         (void*)hk_destroy_swapchain,        (void**)&g_orig_destroy_swapchain },
    };
    enum { N = (int)(sizeof specs / sizeof specs[0]) };
    void* addr[N];
    for (int i = 0; i < N; i++) {
        addr[i] = vdhs_resolve_mapped_symbol(kLoaderNeedle, specs[i].name);
        if (!addr[i]) { LOGE("pt: install %s not in .dynsym", specs[i].name); return -1; }
        LOGI("pt: install %s @ +0x%lx", specs[i].name, (unsigned long)((uintptr_t)addr[i] - base));
    }
    int ok = 0;
    for (int i = 0; i < N; i++) {
        void* stub = shadowhook_hook_func_addr(addr[i], specs[i].hook, specs[i].orig);
        if (!stub) {
            int e = shadowhook_get_errno();
            LOGE("pt: hook %s failed: %d %s", specs[i].name, e, shadowhook_to_errmsg(e));
            continue;
        }
        ok++;
    }
    LOGI("pt: swapchain hooks %d/%d", ok, N);
    return ok == N ? 0 : -1;
}
