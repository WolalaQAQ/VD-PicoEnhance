// VD compat layer, hand tracking part (see docs/ARCHITECTURE.md).
//
//   - XR._supportedExtensions injection (plan A): after VD's xrCreateInstance
//     succeeds, add XR_EXT_hand_tracking / XR_FB_hand_tracking_aim /
//     XR_FB_hand_tracking_mesh (Extension 27/50/52) to VD's managed HashSet via
//     the Mono embedding API. VD filters the runtime list through the PICO
//     whitelist (XR.cs:1140) before enumerate, so that is the only point after
//     the filter and before Session.InitializeHandTracking (Session.cs:1600).
//   - xrGetHandMeshFB served by the layer from hand_mesh_fb.bin (PICO XRShell
//     mesh converted offline by tools/mesh/xrshell_mesh.py; never shipped).
//     No blob -> 52 is not added, so VD never asks for a mesh.
//   - xrCreateHandTrackerEXT / xrDestroyHandTrackerEXT wrappers: handle -> hand.
//   - xrLocateHandJointsEXT wrapper: FB structs are unlinked from the next chain
//     for the runtime call, then XrHandTrackingScaleFB is filled (1.0) and
//     XrHandTrackingAimStateFB is synthesised from the joints (pinch,
//     shoulder->index-proximal ray, palm-toward-headset menu gesture).
#define _GNU_SOURCE
#include <dlfcn.h>
#include <math.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <android/log.h>

#include "vdhs_layer.h"

#define TAG "vdhs-layer"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

void* vdhs_resolve_mapped_symbol(const char* lib, const char* name);   // vdhs_payload.c

// ---------------------------------------------------------------- OpenXR subset
typedef int32_t XrResult;
typedef uint32_t XrBool32;
typedef uint64_t XrHandle;
typedef int64_t XrTime;
typedef void (*PFN_xrVoidFunction)(void);

#define XR_ERROR_VALIDATION_FAILURE (-1)
#define XR_ERROR_HANDLE_INVALID (-12)
#define XR_ERROR_SIZE_INSUFFICIENT (-11)

#define XR_TYPE_HAND_TRACKING_SCALE_FB     1000110003u
#define XR_TYPE_HAND_TRACKING_AIM_STATE_FB 1000111001u

typedef struct { float x, y, z, w; } XrQuaternionf;
typedef struct { float x, y, z; } XrVector3f;
typedef struct { XrQuaternionf orientation; XrVector3f position; } XrPosef;

typedef struct { uint32_t type; void* next; } XrBaseOut;

typedef struct { uint32_t type; const void* next; uint32_t hand; uint32_t handJointSet; } XrHandTrackerCreateInfoEXT;
typedef struct { uint32_t type; const void* next; XrHandle baseSpace; XrTime time; } XrHandJointsLocateInfoEXT;
typedef struct { uint64_t locationFlags; XrPosef pose; float radius; } XrHandJointLocationEXT;
typedef struct {
    uint32_t type; void* next; XrBool32 isActive; uint32_t jointCount;
    XrHandJointLocationEXT* jointLocations;
} XrHandJointLocationsEXT;
typedef struct {
    uint32_t type; void* next;
    float sensorOutput, currentOutput; XrBool32 overrideHandScale; float overrideValueInput;
} XrHandTrackingScaleFB;

typedef struct {
    uint32_t type; void* next;
    uint32_t jointCapacityInput, jointCountOutput;
    XrPosef* jointBindPoses; float* jointRadii; int32_t* jointParents;
    uint32_t vertexCapacityInput, vertexCountOutput;
    XrVector3f* vertexPositions; XrVector3f* vertexNormals; float* vertexUVs;
    int16_t* vertexBlendIndices; float* vertexBlendWeights;
    uint32_t indexCapacityInput, indexCountOutput;
    int16_t* indices;
} XrHandTrackingMeshFB;

typedef XrResult (*PFN_create_ht)(XrHandle, const XrHandTrackerCreateInfoEXT*, XrHandle*);
typedef XrResult (*PFN_destroy_ht)(XrHandle);
typedef XrResult (*PFN_locate_hj)(XrHandle, const XrHandJointsLocateInfoEXT*, XrHandJointLocationsEXT*);

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// ---------------------------------------------------------------- mesh blob
// Layout: tools/mesh/xrshell_mesh.py docstring ("Blob layout").
typedef struct {
    uint32_t hand, joint_count, vertex_count, index_count;
    uint32_t off[9];   // bind poses, radii, parents, pos, nrm, uv, blend idx, blend w, indices
    uint32_t reserved[3];
} blob_hand;

static uint8_t* g_blob;
static const blob_hand* g_blob_hand[3];   // indexed by XrHandEXT (1 = left, 2 = right)
static int g_blob_state;                  // 0 = not tried, 1 = ok, -1 = unusable
static pthread_mutex_t g_blob_lock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t crc32_ieee(const uint8_t* p, size_t n) {
    static uint32_t table[256];
    if (!table[1]) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

// Element size of blob array k (see blob_hand.off).
static size_t blob_elem_size(int k) {
    static const size_t sz[9] = { 28, 4, 4, 12, 12, 8, 8, 16, 2 };
    return sz[k];
}

static int blob_validate(const uint8_t* b, size_t len) {
    if (len < 32 + 2 * 64 || memcmp(b, "VDHM", 4) != 0) return -1;
    uint32_t ver, nh, size, crc, hsz;
    memcpy(&ver, b + 4, 4); memcpy(&nh, b + 8, 4); memcpy(&size, b + 12, 4);
    memcpy(&crc, b + 16, 4); memcpy(&hsz, b + 20, 4);
    if (ver != 1 || nh != 2 || size != len || hsz != 64) return -2;
    if (crc32_ieee(b + 32, len - 32) != crc) return -3;
    for (int i = 0; i < 2; i++) {
        const blob_hand* h = (const blob_hand*)(b + 32 + 64 * i);
        if (h->hand != (uint32_t)(i + 1) || h->joint_count != 26 || h->vertex_count == 0 ||
            h->index_count == 0 || h->index_count % 3) return -4;
        for (int k = 0; k < 9; k++) {
            size_t n = k < 3 ? h->joint_count : k < 8 ? h->vertex_count : h->index_count;
            if (h->off[k] % 16 || (size_t)h->off[k] + n * blob_elem_size(k) > len) return -5;
        }
        const int16_t* idx = (const int16_t*)(b + h->off[8]);
        for (uint32_t j = 0; j < h->index_count; j++)
            if (idx[j] < 0 || (uint32_t)idx[j] >= h->vertex_count) return -6;
    }
    return 0;
}

int vdhs_hand_mesh_ready(void) {
    if (g_blob_state) return g_blob_state > 0;
    pthread_mutex_lock(&g_blob_lock);
    if (!g_blob_state) {
        char path[1100] = "";
        Dl_info info;
        if (dladdr((void*)vdhs_hand_mesh_ready, &info) && info.dli_fname) {
            snprintf(path, sizeof path, "%s", info.dli_fname);
            char* slash = strrchr(path, '/');
            if (slash) snprintf(slash + 1, sizeof path - (size_t)(slash + 1 - path), "hand_mesh_fb.bin");
        }
        int state = -1;
        FILE* f = path[0] ? fopen(path, "rb") : NULL;
        if (!f) {
            LOGI("mesh: %s not present, XR_FB_hand_tracking_mesh disabled", path);
        } else {
            fseek(f, 0, SEEK_END);
            long len = ftell(f);
            rewind(f);
            uint8_t* b = len > 0 ? (uint8_t*)malloc((size_t)len) : NULL;
            if (b && fread(b, 1, (size_t)len, f) == (size_t)len) {
                int rc = blob_validate(b, (size_t)len);
                if (rc == 0) {
                    g_blob = b;
                    g_blob_hand[1] = (const blob_hand*)(b + 32);
                    g_blob_hand[2] = (const blob_hand*)(b + 96);
                    state = 1;
                    LOGI("mesh: loaded %s (%ld B) L v=%u i=%u R v=%u i=%u", path, len,
                         g_blob_hand[1]->vertex_count, g_blob_hand[1]->index_count,
                         g_blob_hand[2]->vertex_count, g_blob_hand[2]->index_count);
                } else {
                    LOGE("mesh: %s invalid (rc=%d), XR_FB_hand_tracking_mesh disabled", path, rc);
                }
            }
            if (state < 0) free(b);
            fclose(f);
        }
        g_blob_state = state;
    }
    pthread_mutex_unlock(&g_blob_lock);
    return g_blob_state > 0;
}

// ---------------------------------------------------------------- hand trackers
#define MAX_HT 8
static struct { XrHandle h; uint32_t hand; XrHandle session; } g_ht[MAX_HT];
static pthread_mutex_t g_ht_lock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t tracker_hand_session(XrHandle h, XrHandle* session) {
    uint32_t hand = 0;
    pthread_mutex_lock(&g_ht_lock);
    for (int i = 0; i < MAX_HT; i++) if (g_ht[i].h == h) { hand = g_ht[i].hand; if (session) *session = g_ht[i].session; break; }
    pthread_mutex_unlock(&g_ht_lock);
    return hand;
}
static uint32_t tracker_hand(XrHandle h) { return tracker_hand_session(h, NULL); }

typedef struct {
    uint32_t type; void* next; uint64_t status; XrPosef aimPose;
    float pinchStrengthIndex, pinchStrengthMiddle, pinchStrengthRing, pinchStrengthLittle;
} XrHandTrackingAimStateFB;
static void aim_update(uint32_t hand, XrHandle session, const XrHandJointsLocateInfoEXT* li,
                       const XrHandJointLocationsEXT* loc, XrHandTrackingAimStateFB* aim);

static void mesh_selftest(XrHandle tracker, uint32_t hand);
static int hj_freeze_gate(uint32_t hand, XrHandJointLocationsEXT* loc);
static void hj_diag(uint32_t hand, const XrHandJointLocationsEXT* loc, int frozen_ms, int gated,
                    const XrHandTrackingAimStateFB* aim);
static void snap_update(uint32_t hand, const XrHandJointLocationsEXT* loc, int frozen_ms);

static PFN_create_ht  g_next_create_ht;
static PFN_destroy_ht g_next_destroy_ht;
static PFN_locate_hj  g_next_locate_hj;

static XrResult w_create_ht(XrHandle session, const XrHandTrackerCreateInfoEXT* ci, XrHandle* out) {
    XrResult r = g_next_create_ht(session, ci, out);
    if (r == 0 && ci && out) {
        pthread_mutex_lock(&g_ht_lock);
        int slot = -1;
        for (int i = 0; i < MAX_HT; i++) if (!g_ht[i].h) { slot = i; break; }
        if (slot >= 0) { g_ht[slot].h = *out; g_ht[slot].hand = ci->hand; g_ht[slot].session = session; }
        pthread_mutex_unlock(&g_ht_lock);
        if (slot < 0) LOGE("hand: tracker table full");
    }
    LOGI("hand: xrCreateHandTrackerEXT hand=%u set=%u r=%d h=0x%llx", ci ? ci->hand : 0,
         ci ? ci->handJointSet : 0, r, (unsigned long long)(r == 0 && out ? *out : 0));
    if (r == 0 && ci && out) mesh_selftest(*out, ci->hand);
    return r;
}

static XrResult w_destroy_ht(XrHandle h) {
    XrResult r = g_next_destroy_ht(h);
    pthread_mutex_lock(&g_ht_lock);
    for (int i = 0; i < MAX_HT; i++) if (g_ht[i].h == h) { g_ht[i].h = 0; g_ht[i].hand = 0; g_ht[i].session = 0; }
    pthread_mutex_unlock(&g_ht_lock);
    LOGI("hand: xrDestroyHandTrackerEXT h=0x%llx r=%d", (unsigned long long)h, r);
    return r;
}

// Served entirely by the layer (the runtime has no FB mesh).
static XrResult w_get_hand_mesh(XrHandle tracker, XrHandTrackingMeshFB* m) {
    static volatile int calls;
    int n = __atomic_add_fetch(&calls, 1, __ATOMIC_RELAXED);
    uint32_t hand = tracker_hand(tracker);
    XrResult r = 0;
    if (!m) r = XR_ERROR_VALIDATION_FAILURE;
    else if (hand != 1 && hand != 2) r = XR_ERROR_HANDLE_INVALID;
    else {
        const blob_hand* h = g_blob_hand[hand];
        const uint8_t* b = g_blob;
        m->jointCountOutput = h->joint_count;
        m->vertexCountOutput = h->vertex_count;
        m->indexCountOutput = h->index_count;
        // Each group is either a pure size query (capacity 0) or a full copy.
        if (m->jointCapacityInput) {
            if (m->jointCapacityInput < h->joint_count) r = XR_ERROR_SIZE_INSUFFICIENT;
            else if (!m->jointBindPoses || !m->jointRadii || !m->jointParents) r = XR_ERROR_VALIDATION_FAILURE;
        }
        if (!r && m->vertexCapacityInput) {
            if (m->vertexCapacityInput < h->vertex_count) r = XR_ERROR_SIZE_INSUFFICIENT;
            else if (!m->vertexPositions || !m->vertexNormals || !m->vertexUVs ||
                     !m->vertexBlendIndices || !m->vertexBlendWeights) r = XR_ERROR_VALIDATION_FAILURE;
        }
        if (!r && m->indexCapacityInput) {
            if (m->indexCapacityInput < h->index_count) r = XR_ERROR_SIZE_INSUFFICIENT;
            else if (!m->indices) r = XR_ERROR_VALIDATION_FAILURE;
        }
        if (!r) {
            uint32_t jc = h->joint_count, vc = h->vertex_count, ic = h->index_count;
            if (m->jointCapacityInput) {
                memcpy(m->jointBindPoses, b + h->off[0], jc * blob_elem_size(0));
                memcpy(m->jointRadii,     b + h->off[1], jc * blob_elem_size(1));
                memcpy(m->jointParents,   b + h->off[2], jc * blob_elem_size(2));
            }
            if (m->vertexCapacityInput) {
                memcpy(m->vertexPositions,    b + h->off[3], vc * blob_elem_size(3));
                memcpy(m->vertexNormals,      b + h->off[4], vc * blob_elem_size(4));
                memcpy(m->vertexUVs,          b + h->off[5], vc * blob_elem_size(5));
                memcpy(m->vertexBlendIndices, b + h->off[6], vc * blob_elem_size(6));
                memcpy(m->vertexBlendWeights, b + h->off[7], vc * blob_elem_size(7));
            }
            if (m->indexCapacityInput) memcpy(m->indices, b + h->off[8], ic * blob_elem_size(8));
        }
    }
    LOGI("hand: getHandMesh#%d h=0x%llx hand=%u cap=%u/%u/%u out=%u/%u/%u r=%d", n,
         (unsigned long long)tracker, hand,
         m ? m->jointCapacityInput : 0, m ? m->vertexCapacityInput : 0, m ? m->indexCapacityInput : 0,
         m ? m->jointCountOutput : 0, m ? m->vertexCountOutput : 0, m ? m->indexCountOutput : 0, r);
    return r;
}

// FB structs the runtime does not know are unlinked for the downstream call,
// then relinked; the layer fills them afterwards.
static XrResult w_locate_hj(XrHandle tracker, const XrHandJointsLocateInfoEXT* li, XrHandJointLocationsEXT* loc) {
    enum { MAXU = 4 };
    void** prev_next[MAXU];
    XrBaseOut* node[MAXU];
    int nu = 0;
    XrHandTrackingScaleFB* scale = NULL;
    XrHandTrackingAimStateFB* aim = NULL;
    if (loc) {
        void** link = &loc->next;
        while (*link) {
            XrBaseOut* s = (XrBaseOut*)*link;
            if (s->type == XR_TYPE_HAND_TRACKING_AIM_STATE_FB) {
                // Left in the chain: the PICO runtime fills it itself
                // (xrLocateHandJointsEXT checks type 1000111001 and asks the
                // input service for the system hand ray, the one XRShell draws).
                aim = (XrHandTrackingAimStateFB*)s;
                memset((uint8_t*)aim + offsetof(XrHandTrackingAimStateFB, status), 0,
                       sizeof *aim - offsetof(XrHandTrackingAimStateFB, status));
            } else if (s->type == XR_TYPE_HAND_TRACKING_SCALE_FB && nu < MAXU) {
                scale = (XrHandTrackingScaleFB*)s;
                prev_next[nu] = link; node[nu] = s; nu++;
                *link = s->next;       // unlink; `link` now points at the successor
                continue;
            }
            link = &s->next;
        }
    }
    XrResult r = g_next_locate_hj(tracker, li, loc);
    for (int i = nu - 1; i >= 0; i--) *prev_next[i] = node[i];   // restore in reverse order

    if (r == 0 && scale) {
        scale->sensorOutput = 1.0f;
        scale->currentOutput = scale->overrideHandScale ? scale->overrideValueInput : 1.0f;
    }

    XrHandle session = 0;
    uint32_t hand = tracker_hand_session(tracker, &session);
    int frozen_ms = 0, gated = 0;
    if (r == 0 && loc) {
        frozen_ms = hj_freeze_gate(hand, loc);
        gated = frozen_ms < 0;
        if (gated) frozen_ms = -frozen_ms;
    }
    if (r == 0 && aim) aim_update(hand, session, li, loc, aim);
    if (r == 0 && loc) hj_diag(hand, loc, frozen_ms, gated, aim);
    if (r == 0 && loc) snap_update(hand, loc, frozen_ms);

    // Log the first calls, isActive transitions and a heartbeat every ~5 s.
    static volatile int calls;
    static int last_active[3] = { -1, -1, -1 };
    static int64_t last_ms;
    int n = __atomic_add_fetch(&calls, 1, __ATOMIC_RELAXED);
    int act = (r == 0 && loc) ? (int)loc->isActive : -1;
    int64_t t = now_ms();
    int hi = hand <= 2 ? (int)hand : 0;
    if (n <= 4 || act != last_active[hi] || t - last_ms >= 5000) {
        if (t - last_ms >= 5000) last_ms = t;
        last_active[hi] = act;
        uint64_t wf = (act > 0 && loc->jointCount > 1 && loc->jointLocations) ? loc->jointLocations[1].locationFlags : 0;
        // PALM (joint 0) is VD's reference for its trigger/grip curl
        // (InputStateHandExtensions.UpdateTriggerState: |x| of joints 7-9 relative to 0).
        const XrHandJointLocationEXT* pj = (act > 0 && loc->jointLocations) ? &loc->jointLocations[0] : NULL;
        LOGI("hand: locate#%d hand=%u r=%d isActive=%d wristFlags=0x%llx palmFlags=0x%llx palmQ=(%.2f,%.2f,%.2f,%.2f) "
             "unlinked=%d scale=%s", n, hand, r, act, (unsigned long long)wf,
             (unsigned long long)(pj ? pj->locationFlags : 0), pj ? pj->pose.orientation.x : 0, pj ? pj->pose.orientation.y : 0,
             pj ? pj->pose.orientation.z : 0, pj ? pj->pose.orientation.w : 0, nu, scale ? "1.0" : "-");
    }
    return r;
}

// VD only asks for the mesh once a hand is actually tracked (OpenXRHMD.cs:2034
// after TryGetHandPose), which never happens without the headset on. Replay
// VD's two-call protocol (OpenXRHMD.cs:2127-2160) once per tracker so the
// served path is exercised headless; results are compared with the blob.
static void mesh_selftest(XrHandle tracker, uint32_t hand) {
    if (!vdhs_hand_mesh_ready() || (hand != 1 && hand != 2)) return;
    XrHandTrackingMeshFB m;
    memset(&m, 0, sizeof m);
    m.type = 1000110001u;   // XR_TYPE_HAND_TRACKING_MESH_FB
    XrResult r1 = w_get_hand_mesh(tracker, &m);
    uint32_t jc = m.jointCountOutput, vc = m.vertexCountOutput, ic = m.indexCountOutput;
    size_t sz[9] = { jc * 28, jc * 4, jc * 4, vc * 12, vc * 12, vc * 8, vc * 8, vc * 16, ic * 2 };
    void* buf[9] = { 0 };
    int ok = r1 == 0 && jc && vc && ic;
    for (int k = 0; ok && k < 9; k++) ok = (buf[k] = malloc(sz[k])) != NULL;
    XrResult r2 = -999;
    if (ok) {
        m.jointCapacityInput = jc; m.vertexCapacityInput = vc; m.indexCapacityInput = ic;
        m.jointBindPoses = buf[0]; m.jointRadii = buf[1]; m.jointParents = buf[2];
        m.vertexPositions = buf[3]; m.vertexNormals = buf[4]; m.vertexUVs = buf[5];
        m.vertexBlendIndices = buf[6]; m.vertexBlendWeights = buf[7]; m.indices = buf[8];
        r2 = w_get_hand_mesh(tracker, &m);
        const blob_hand* h = g_blob_hand[hand];
        for (int k = 0; r2 == 0 && ok && k < 9; k++) ok = !memcmp(buf[k], g_blob + h->off[k], sz[k]);
    }
    for (int k = 0; k < 9; k++) free(buf[k]);
    LOGI("hand: mesh selftest hand=%u r1=%d r2=%d j/v/i=%u/%u/%u match=%d", hand, r1, r2, jc, vc, ic,
         ok && r2 == 0);
}

// ---------------------------------------------------------------- aim synthesis + menu gesture
// Pinch metric / aim ray follow the approach in StereoKit input_hand.cpp and
// hand_oxr_articulated.cpp (MIT) and Monado ht_ctrl_emu.cpp (BSL-1.0); code is
// written from scratch, only thresholds/geometry are taken from there.
enum {
    J_PALM = 0, J_WRIST = 1, J_THUMB_TIP = 5, J_INDEX_PROX = 7, J_INDEX_TIP = 10,
    J_MIDDLE_PROX = 12, J_MIDDLE_TIP = 15, J_RING_PROX = 17, J_RING_TIP = 20,
    J_LITTLE_PROX = 22, J_LITTLE_TIP = 25,
};
#define LOC_POS_VALID 0x2ull
#define AIM_COMPUTED  0x1ull
#define AIM_VALID     0x2ull
#define AIM_PINCH(i)  (0x4ull << (i))   // index, middle, ring, little
#define AIM_SYSTEM    0x40ull
#define AIM_DOMINANT  0x80ull
#define AIM_MENU      0x100ull

typedef struct { uint32_t type; const void* next; int32_t referenceSpaceType; XrPosef pose; } XrReferenceSpaceCreateInfo;
typedef struct { uint32_t type; void* next; uint64_t locationFlags; XrPosef pose; } XrSpaceLocation;
typedef XrResult (*PFN_create_ref_space)(XrHandle, const XrReferenceSpaceCreateInfo*, XrHandle*);
typedef XrResult (*PFN_locate_space)(XrHandle, XrHandle, XrTime, XrSpaceLocation*);

// Thresholds; overridable in <payload>/hand_gesture.txt as `key = value`
// lines ('#' comments), read once per process (VD restart applies changes).
#define PINCH_ON_CM_DEFAULT   0.4f
#define PINCH_OFF_CM_DEFAULT  0.8f
#define PINCH_FULL_CM_DEFAULT 3.0f
static struct {
    float pinch_on_cm, pinch_off_cm, pinch_full_cm;
    float face_on_deg, face_off_deg;
    float menu_hold_ms, menu_cooldown_ms;
    float ext_dot, curl_dot;
    float aim_smooth;          // 0 = no smoothing, 0.9 = heavy; exponential per call
    // Menu gesture is stricter than plain "facing" (user test: too easy to
    // trigger on the left hand, index often only half bent at d<1 cm).
    float menu_face_deg;       // palm must face the headset within this angle
    float menu_pinch_cm;       // index-thumb distance to start the menu pinch
    float menu_ext_dot;        // middle/ring/little straightness
    float aim_shoulder;        // 1 = shoulder->hand ray, 0 = wrist->index-proximal direction
    float aim_pitch_deg;       // extra pitch added to the ray (+ = up)
    float aim_runtime;         // 1 = use the runtime (PICO system) aim pose when it is valid
    float pt_style;            // 1 = set textureOpacityFactor=1 on VD's projected (hands) layer
    float pt_bg_fix;           // 1 = re-resume a running background layer after another is paused
    float gaze_log;            // 1 = rate-limited EyeGazeSpace xrLocateSpace log
    float pt_split;            // 1 = one projected layer per hand geometry (PICO keeps one per layer)
    // Pointer-only stabiliser (vdhs_layer.c gaze_filter): One-Euro on the
    // head-relative gaze direction, hold on invalid samples, one-sample
    // confirmation of jumps larger than gaze_jump_deg. Streaming bypasses all.
    float gaze_filter, gaze_mincutoff, gaze_beta, gaze_hold_ms, gaze_jump_deg;
    // SteamVR trigger diagnosis: per-hand log period of VD's
    // trigger/grip curl formula (0 = off), and the frozen-joints gate
    // (report isActive=0 once the runtime repeats identical joints this long; 0 = off).
    float hj_diag_ms, hj_freeze_ms;
    // Zero-copy alpha hole in the VR projection swapchain. pt_hole
    // draws the joint balls (scissor-limited, alpha only); pt_alpha_only only
    // ORs 0x6 into the projection layer flags to probe whether VD's alpha is 1
    // outside the hands. Drawing defaults on; the diagnostic stays off.
    float pt_hole, pt_alpha_only;
    float pt_flip_y;           // 1 = mirror the projection in y (VD's VerticalFlip; default on)
    float pt_margin_px;        // scissor rectangle expansion, pixels
    float pt_disc_r;           // sphere radius, meters (must not exceed VD's 0.1 window: excess draws black)
    float pt_max_age_ms;       // locate snapshot older than this is not drawn
    float pt_follow_settings;  // 1 = only in SteamVR with VD's "Passthrough hands" on (vdhs_hand_pt_gate)
    float gaze_vd_fix;         // 1 = pre-compensate verified VD pointer calls only (never GetEyeState/streaming)
} G = {
    .pinch_on_cm = PINCH_ON_CM_DEFAULT,
    .pinch_off_cm = PINCH_OFF_CM_DEFAULT,
    .pinch_full_cm = PINCH_FULL_CM_DEFAULT,
    .face_on_deg = 40.0f, .face_off_deg = 55.0f,
    .menu_hold_ms = 400.0f, .menu_cooldown_ms = 700.0f,
    .ext_dot = 0.7f, .curl_dot = 0.2f, .aim_smooth = 0.5f,
    .menu_face_deg = 40.0f, .menu_pinch_cm = 0.6f, .menu_ext_dot = 0.70f,
    .aim_shoulder = 1.0f, .aim_pitch_deg = 0.0f, .aim_runtime = 1.0f,
    .pt_style = 0.0f, .pt_bg_fix = 1.0f, .pt_split = 1.0f,
    .gaze_log = 0.0f,
    .gaze_filter = 1.0f, .gaze_mincutoff = 1.0f, .gaze_beta = 0.5f,
    .gaze_hold_ms = 150.0f, .gaze_jump_deg = 25.0f,
    .hj_diag_ms = 0.0f, .hj_freeze_ms = 300.0f,
    .pt_hole = 1.0f, .pt_alpha_only = 0.0f,
    .pt_flip_y = 1.0f, .pt_margin_px = 8.0f, .pt_disc_r = 0.1f,
    .pt_max_age_ms = 300.0f, .pt_follow_settings = 1.0f, .gaze_vd_fix = 1.0f
};

static void payload_path(const char* file, char* out, size_t cap) {
    out[0] = 0;
    Dl_info info;
    if (!dladdr((void*)payload_path, &info) || !info.dli_fname) return;
    snprintf(out, cap, "%s", info.dli_fname);
    char* slash = strrchr(out, '/');
    if (slash) snprintf(slash + 1, cap - (size_t)(slash + 1 - out), "%s", file);
}

static void gesture_config_load(void) {
    char path[1100];
    payload_path("hand_gesture.txt", path, sizeof path);
    FILE* f = path[0] ? fopen(path, "r") : NULL;
    if (f) {
        static const struct { const char* k; float* v; } keys[] = {
            { "pinch_on_cm", &G.pinch_on_cm }, { "pinch_off_cm", &G.pinch_off_cm }, { "pinch_full_cm", &G.pinch_full_cm },
            { "face_on_deg", &G.face_on_deg }, { "face_off_deg", &G.face_off_deg },
            { "menu_hold_ms", &G.menu_hold_ms }, { "menu_cooldown_ms", &G.menu_cooldown_ms },
            { "ext_dot", &G.ext_dot }, { "curl_dot", &G.curl_dot }, { "aim_smooth", &G.aim_smooth },
            { "menu_face_deg", &G.menu_face_deg }, { "menu_pinch_cm", &G.menu_pinch_cm },
            { "menu_ext_dot", &G.menu_ext_dot }, { "aim_shoulder", &G.aim_shoulder },
            { "aim_pitch_deg", &G.aim_pitch_deg }, { "aim_runtime", &G.aim_runtime },
            { "pt_style", &G.pt_style }, { "pt_bg_fix", &G.pt_bg_fix }, { "gaze_log", &G.gaze_log },
            { "pt_split", &G.pt_split }, { "gaze_filter", &G.gaze_filter },
            { "gaze_mincutoff", &G.gaze_mincutoff }, { "gaze_beta", &G.gaze_beta },
            { "gaze_hold_ms", &G.gaze_hold_ms }, { "gaze_jump_deg", &G.gaze_jump_deg },
            { "hj_diag_ms", &G.hj_diag_ms }, { "hj_freeze_ms", &G.hj_freeze_ms },
            { "pt_hole", &G.pt_hole }, { "pt_alpha_only", &G.pt_alpha_only },
            { "pt_flip_y", &G.pt_flip_y }, { "pt_margin_px", &G.pt_margin_px },
            { "pt_disc_r", &G.pt_disc_r }, { "pt_max_age_ms", &G.pt_max_age_ms },
            { "pt_follow_settings", &G.pt_follow_settings }, { "gaze_vd_fix", &G.gaze_vd_fix },
        };
        char line[256];
        while (fgets(line, sizeof line, f)) {
            char k[64]; float v;
            if (line[0] == '#' || sscanf(line, " %63[a-z_] = %f", k, &v) != 2) continue;
            for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) if (!strcmp(k, keys[i].k)) *keys[i].v = v;
        }
        fclose(f);
    }
    // Keep hysteresis ordered and the strength denominator positive. Reject
    // the whole group so partial overrides cannot leave inconsistent thresholds.
    if (!isfinite(G.pinch_on_cm) || !isfinite(G.pinch_off_cm) || !isfinite(G.pinch_full_cm) ||
        G.pinch_on_cm < 0 || G.pinch_on_cm >= G.pinch_off_cm ||
        G.pinch_off_cm > G.pinch_full_cm) {
        LOGE("aim: invalid pinch thresholds %g/%g/%gcm; using defaults for all three",
             G.pinch_on_cm, G.pinch_off_cm, G.pinch_full_cm);
        G.pinch_on_cm = PINCH_ON_CM_DEFAULT;
        G.pinch_off_cm = PINCH_OFF_CM_DEFAULT;
        G.pinch_full_cm = PINCH_FULL_CM_DEFAULT;
    }
    LOGI("aim: config %s pinch=%.1f/%.1f/%.1fcm face=%.0f/%.0fdeg menu=%.0f/%.0fms ext=%.2f curl=%.2f smooth=%.2f "
         "menu_face=%.0fdeg menu_pinch=%.1fcm menu_ext=%.2f shoulder=%.2f pitch=%.0fdeg aim_runtime=%.0f "
         "pt_style=%.0f pt_bg_fix=%.0f gaze_log=%.0f pt_split=%.0f gaze=%.0f/%.2fHz/%.2f/%.0fms/%.0fdeg "
         "hj_diag=%.0fms hj_freeze=%.0fms pt_hole=%.0f pt_alpha_only=%.0f pt_flip_y=%.0f margin=%.0fpx disc_r=%.3fm maxage=%.0fms "
         "pt_follow=%.0f gaze_vd_fix=%.0f",
         f ? "loaded" : "defaults", G.pinch_on_cm, G.pinch_off_cm, G.pinch_full_cm, G.face_on_deg, G.face_off_deg,
         G.menu_hold_ms, G.menu_cooldown_ms, G.ext_dot, G.curl_dot, G.aim_smooth,
         G.menu_face_deg, G.menu_pinch_cm, G.menu_ext_dot, G.aim_shoulder, G.aim_pitch_deg, G.aim_runtime,
         G.pt_style, G.pt_bg_fix, G.gaze_log, G.pt_split,
         G.gaze_filter, G.gaze_mincutoff, G.gaze_beta, G.gaze_hold_ms, G.gaze_jump_deg,
         G.hj_diag_ms, G.hj_freeze_ms, G.pt_hole, G.pt_alpha_only, G.pt_flip_y, G.pt_margin_px, G.pt_disc_r,
         G.pt_max_age_ms, G.pt_follow_settings, G.gaze_vd_fix);
}

static pthread_once_t g_cfg_once = PTHREAD_ONCE_INIT;

float vdhs_hand_cfg(const char* key) {
    pthread_once(&g_cfg_once, gesture_config_load);
    if (!strcmp(key, "pt_style")) return G.pt_style;
    if (!strcmp(key, "pt_bg_fix")) return G.pt_bg_fix;
    if (!strcmp(key, "gaze_log")) return G.gaze_log;
    if (!strcmp(key, "pt_split")) return G.pt_split;
    if (!strcmp(key, "gaze_filter")) return G.gaze_filter;
    if (!strcmp(key, "gaze_mincutoff")) return G.gaze_mincutoff;
    if (!strcmp(key, "gaze_beta")) return G.gaze_beta;
    if (!strcmp(key, "gaze_hold_ms")) return G.gaze_hold_ms;
    if (!strcmp(key, "gaze_jump_deg")) return G.gaze_jump_deg;
    if (!strcmp(key, "pt_hole")) return G.pt_hole;
    if (!strcmp(key, "pt_alpha_only")) return G.pt_alpha_only;
    if (!strcmp(key, "pt_flip_y")) return G.pt_flip_y;
    if (!strcmp(key, "pt_margin_px")) return G.pt_margin_px;
    if (!strcmp(key, "pt_disc_r")) return G.pt_disc_r;
    if (!strcmp(key, "pt_max_age_ms")) return G.pt_max_age_ms;
    if (!strcmp(key, "pt_follow_settings")) return G.pt_follow_settings;
    if (!strcmp(key, "gaze_vd_fix")) return G.gaze_vd_fix;
    return 0;
}

// Latest valid locate snapshot, consumed by vdhs_pt.c's alpha hole. A frozen
// sample is only dropped once the joints have been bit-identical for
// hj_freeze_ms (or 500 ms when the gate is off): a still hand repeats a few
// frames and must not blank the hole, while a real freeze (headset off,
// s7-hmd3) does. Stale data is additionally bounded by pt_max_age_ms.
static struct {
    int valid;
    int count;
    int64_t t_ms;
    vdhs_pt_joint j[26];
} g_snap[3];
static pthread_mutex_t g_snap_lock = PTHREAD_MUTEX_INITIALIZER;

static void snap_update(uint32_t hand, const XrHandJointLocationsEXT* loc, int frozen_ms) {
    if (hand != 1 && hand != 2) return;
    int fs = G.hj_freeze_ms > 0 ? (int)G.hj_freeze_ms : 500;
    pthread_mutex_lock(&g_snap_lock);
    if (!loc->isActive || !loc->jointLocations || loc->jointCount < 26 || frozen_ms >= fs) {
        g_snap[hand].valid = 0;
    } else {
        int n = loc->jointCount > 26 ? 26 : (int)loc->jointCount;
        for (int i = 0; i < n; i++) {
            const XrHandJointLocationEXT* j = &loc->jointLocations[i];
            g_snap[hand].j[i].x = j->pose.position.x;
            g_snap[hand].j[i].y = j->pose.position.y;
            g_snap[hand].j[i].z = j->pose.position.z;
            g_snap[hand].j[i].radius = j->radius;
            g_snap[hand].j[i].flags = j->locationFlags;
        }
        g_snap[hand].count = n;
        g_snap[hand].t_ms = now_ms();
        g_snap[hand].valid = 1;
    }
    pthread_mutex_unlock(&g_snap_lock);
}

int vdhs_pt_hand_snapshot(int hand, vdhs_pt_joint* out, int max) {
    if ((hand != 1 && hand != 2) || !out || max <= 0) return 0;
    pthread_once(&g_cfg_once, gesture_config_load);
    int n = 0;
    pthread_mutex_lock(&g_snap_lock);
    if (g_snap[hand].valid && now_ms() - g_snap[hand].t_ms <= (int64_t)G.pt_max_age_ms) {
        n = g_snap[hand].count < max ? g_snap[hand].count : max;
        memcpy(out, g_snap[hand].j, (size_t)n * sizeof *out);
    }
    pthread_mutex_unlock(&g_snap_lock);
    return n;
}

typedef XrVector3f V3;
static V3 v3(float x, float y, float z) { V3 r = { x, y, z }; return r; }
static V3 vsub(V3 a, V3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static V3 vadd(V3 a, V3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static V3 vscale(V3 a, float s) { return v3(a.x * s, a.y * s, a.z * s); }
static float vdot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static V3 vcross(V3 a, V3 b) { return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
static float vlen(V3 a) { return sqrtf(vdot(a, a)); }
static V3 vnorm(V3 a) { float l = vlen(a); return l > 1e-6f ? vscale(a, 1.0f / l) : v3(0, 0, 0); }
static V3 qrot(XrQuaternionf q, V3 v) {   // v' = q v q*
    V3 u = v3(q.x, q.y, q.z);
    V3 t = vscale(vcross(u, v), 2.0f);
    return vadd(vadd(v, vscale(t, q.w)), vcross(u, t));
}
// Quaternion whose -Z axis is `fwd` and whose +Y is as close to `up` as possible.
static XrQuaternionf quat_look(V3 fwd, V3 up) {
    V3 z = vscale(fwd, -1.0f);
    V3 x = vnorm(vcross(up, z));
    if (vlen(x) < 0.5f) x = vnorm(vcross(v3(0, 0, 1), z));
    V3 y = vcross(z, x);
    float m00 = x.x, m11 = y.y, m22 = z.z, tr = m00 + m11 + m22;
    XrQuaternionf q;
    if (tr > 0) {
        float s = sqrtf(tr + 1.0f) * 2;
        q.w = 0.25f * s; q.x = (y.z - z.y) / s; q.y = (z.x - x.z) / s; q.z = (x.y - y.x) / s;
    } else if (m00 > m11 && m00 > m22) {
        float s = sqrtf(1.0f + m00 - m11 - m22) * 2;
        q.w = (y.z - z.y) / s; q.x = 0.25f * s; q.y = (y.x + x.y) / s; q.z = (z.x + x.z) / s;
    } else if (m11 > m22) {
        float s = sqrtf(1.0f + m11 - m00 - m22) * 2;
        q.w = (z.x - x.z) / s; q.x = (y.x + x.y) / s; q.y = 0.25f * s; q.z = (z.y + y.z) / s;
    } else {
        float s = sqrtf(1.0f + m22 - m00 - m11) * 2;
        q.w = (x.y - y.x) / s; q.x = (z.x + x.z) / s; q.y = (z.y + y.z) / s; q.z = 0.25f * s;
    }
    return q;
}

typedef struct {
    int init;
    int pinching[4];
    int facing;
    int64_t menu_start_ms;      // 0 = not holding
    int menu_pressed;
    int64_t menu_release_ms;
    V3 origin, dir;             // smoothed aim ray
    int last_mode;              // for logging: 0 normal, 1 facing, 2 facing+menu, 3 fist, -1 invalid
} hand_state;
static hand_state g_hs[3];
static volatile int g_menu_any;   // either hand holds MENU_PRESSED (reported on the left hand)

static PFN_create_ref_space g_create_ref_space;
static PFN_locate_space     g_locate_space;
static XrHandle g_view_session, g_view_space;

static int head_pose(XrHandle session, const XrHandJointsLocateInfoEXT* li, XrPosef* out) {
    if (!li || !session) return 0;
    if (!g_create_ref_space) {
        g_create_ref_space = (PFN_create_ref_space)vdhs_resolve_mapped_symbol("/lib/arm64/libopenxr_loader.so", "xrCreateReferenceSpace");
        g_locate_space = (PFN_locate_space)vdhs_resolve_mapped_symbol("/lib/arm64/libopenxr_loader.so", "xrLocateSpace");
        LOGI("aim: loader xrCreateReferenceSpace=%p xrLocateSpace=%p", (void*)g_create_ref_space, (void*)g_locate_space);
        if (!g_create_ref_space || !g_locate_space) return 0;
    }
    if (!g_locate_space) return 0;
    if (g_view_session != session) {
        XrReferenceSpaceCreateInfo ci;
        memset(&ci, 0, sizeof ci);
        ci.type = 37;                 // XR_TYPE_REFERENCE_SPACE_CREATE_INFO
        ci.referenceSpaceType = 1;    // XR_REFERENCE_SPACE_TYPE_VIEW
        ci.pose.orientation.w = 1.0f;
        XrHandle sp = 0;
        XrResult r = g_create_ref_space(session, &ci, &sp);
        LOGI("aim: VIEW space for session 0x%llx r=%d", (unsigned long long)session, r);
        g_view_session = session;
        g_view_space = r == 0 ? sp : 0;
    }
    if (!g_view_space) return 0;
    XrSpaceLocation sl;
    memset(&sl, 0, sizeof sl);
    sl.type = 42;                     // XR_TYPE_SPACE_LOCATION
    if (g_locate_space(g_view_space, li->baseSpace, li->time, &sl) != 0) return 0;
    if ((sl.locationFlags & 0x3) != 0x3) return 0;   // orientation + position valid
    *out = sl.pose;
    return 1;
}

static V3 jp(const XrHandJointLocationEXT* j, int i) { return j[i].pose.position; }

// Straightness of a finger: cos of the angle between wrist->proximal and proximal->tip.
static float finger_dot(const XrHandJointLocationEXT* j, int prox, int tip) {
    return vdot(vnorm(vsub(jp(j, prox), jp(j, J_WRIST))), vnorm(vsub(jp(j, tip), jp(j, prox))));
}

// ---------------------------------------------------------------- SteamVR trigger diagnosis
// Frozen-joints detector. The runtime keeps isActive=1 with bit-identical
// joints after the headset is taken off (s7-hmd3). Returns how long (ms) the
// joints have been unchanged; negative when hj_freeze_ms gated the sample
// (isActive forced to 0 so VD drops the hand, like Quest does).
static int hj_freeze_gate(uint32_t hand, XrHandJointLocationsEXT* loc) {
    pthread_once(&g_cfg_once, gesture_config_load);
    static XrPosef last[3][26];
    static int64_t changed_ms[3];
    if (hand != 1 && hand != 2) return 0;
    int64_t t = now_ms();
    if (!loc->isActive || !loc->jointLocations || loc->jointCount < 26) { changed_ms[hand] = t; return 0; }
    const XrHandJointLocationEXT* j = loc->jointLocations;
    int same = 1;
    for (int i = 0; same && i < 26; i++) {
        const float* a = (const float*)&j[i].pose;
        const float* b = (const float*)&last[hand][i];
        for (int k = 0; k < 7; k++) if (fabsf(a[k] - b[k]) > 1e-6f) { same = 0; break; }
    }
    if (!same) {
        for (int i = 0; i < 26; i++) last[hand][i] = j[i].pose;
        changed_ms[hand] = t;
        return 0;
    }
    int frozen = (int)(t - changed_ms[hand]);
    if (G.hj_freeze_ms > 0 && frozen >= (int)G.hj_freeze_ms) {
        loc->isActive = 0;
        for (int i = 0; i < 26; i++) loc->jointLocations[i].locationFlags = 0;
        return frozen > 0 ? -frozen : -1;
    }
    return frozen;
}

// q_ref^-1 * q (Hamilton): q expressed in the frame of q_ref. This is what
// VD's Xenko Quaternion.Multiply(ref q, ref inv(q_ref)) evaluates to.
static XrQuaternionf q_rel(XrQuaternionf ref, XrQuaternionf q) {
    float ax = -ref.x, ay = -ref.y, az = -ref.z, aw = ref.w;   // unit quaternion: inverse = conjugate
    XrQuaternionf r;
    r.x = aw * q.x + ax * q.w + ay * q.z - az * q.y;
    r.y = aw * q.y - ax * q.z + ay * q.w + az * q.x;
    r.z = aw * q.z + ax * q.y - ay * q.x + az * q.w;
    r.w = aw * q.w - ax * q.x - ay * q.y - az * q.z;
    return r;
}

// VD InputStateHandExtensions.UpdateTriggerState / UpdateGripState, verbatim.
static float vd_curl3(const XrHandJointLocationEXT* j, int a, float* parts) {
    float s = 0;
    for (int k = 0; k < 3; k++) {
        float x = fabsf(q_rel(j[0].pose.orientation, j[a + k].pose.orientation).x);
        if (parts) parts[k] = x;
        s += x;
    }
    return s * 0.4f;
}
static float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

static void hj_diag(uint32_t hand, const XrHandJointLocationsEXT* loc, int frozen_ms, int gated,
                    const XrHandTrackingAimStateFB* aim) {
    if (G.hj_diag_ms <= 0 || (hand != 1 && hand != 2)) return;
    static int64_t last_ms[3];
    int64_t t = now_ms();
    if (t - last_ms[hand] < (int64_t)G.hj_diag_ms) return;
    const XrHandJointLocationEXT* j = loc->jointLocations;
    if (!j || loc->jointCount < 26 || (!loc->isActive && !gated)) return;
    last_ms[hand] = t;
    float ix[3];
    float trig = clamp01(vd_curl3(j, 7, ix));
    float g0 = clamp01((vd_curl3(j, 12, NULL) - 0.9f) * 10.0f);
    float g1 = clamp01((vd_curl3(j, 17, NULL) - 0.9f) * 10.0f);
    float g2 = clamp01((vd_curl3(j, 22, NULL) - 0.9f) * 10.0f);
    float grip = (g0 + g1 + g2) / 3.0f;
    // Geometric ground truth: straightness of index / middle (1 = straight, <0.2 = curled).
    float fd_i = finger_dot(j, J_INDEX_PROX, J_INDEX_TIP), fd_m = finger_dot(j, J_MIDDLE_PROX, J_MIDDLE_TIP);
    float d_index = (vlen(vsub(jp(j, J_INDEX_TIP), jp(j, J_THUMB_TIP))) - j[J_INDEX_TIP].radius - j[J_THUMB_TIP].radius) * 100.0f;
    // Local-axis convention check: in OpenXR every joint's -Z points to its
    // child and PALM's frame matches the proximal ones for a flat hand, so
    // rel(7) should be ~identity with the hand open. Also report the angle
    // between PALM -Z and WRIST->MIDDLE_PROX, and PALM +Y against the palm normal.
    XrQuaternionf r7 = q_rel(j[0].pose.orientation, j[7].pose.orientation);
    XrQuaternionf r1 = q_rel(j[1].pose.orientation, j[0].pose.orientation);   // PALM in WRIST frame
    V3 palm_mz = qrot(j[0].pose.orientation, v3(0, 0, -1));
    V3 wm = vnorm(vsub(jp(j, J_MIDDLE_PROX), jp(j, J_WRIST)));
    V3 i_mz = qrot(j[8].pose.orientation, v3(0, 0, -1));
    V3 i_dir = vnorm(vsub(jp(j, 9), jp(j, 8)));
    const float R = 57.29578f;
    float a_palm = acosf(fmaxf(-1.0f, fminf(1.0f, vdot(palm_mz, wm)))) * R;
    float a_idx = acosf(fmaxf(-1.0f, fminf(1.0f, vdot(i_mz, i_dir)))) * R;
    LOGI("hj: hand=%u act=%d gated=%d frozen=%dms trig=%.2f%s grip=%.2f%s ix=%.2f/%.2f/%.2f g=%.2f/%.2f/%.2f "
         "fd_i=%.2f fd_m=%.2f d_index=%.1fcm pinchS=%.2f r7=(%.2f,%.2f,%.2f,%.2f) palm_in_wrist=(%.2f,%.2f,%.2f,%.2f) "
         "palmZ_vs_wm=%.0fdeg idxZ_vs_bone=%.0fdeg",
         hand, (int)loc->isActive, gated, frozen_ms, trig, trig >= 1.0f - 1e-6f ? "!" : "", grip,
         grip >= 1.0f - 1e-6f ? "!" : "", ix[0], ix[1], ix[2], g0, g1, g2, fd_i, fd_m, d_index,
         aim ? aim->pinchStrengthIndex : -1.0f, r7.x, r7.y, r7.z, r7.w, r1.x, r1.y, r1.z, r1.w, a_palm, a_idx);
}

static void aim_update(uint32_t hand, XrHandle session, const XrHandJointsLocateInfoEXT* li,
                       const XrHandJointLocationsEXT* loc, XrHandTrackingAimStateFB* aim) {
    pthread_once(&g_cfg_once, gesture_config_load);
    if (hand != 1 && hand != 2) return;
    hand_state* hs = &g_hs[hand];
    // What the runtime wrote (it was left in the chain). Its pose is the PICO
    // system hand ray; status/pinch are recomputed below so the menu gesture
    // and pinch thresholds stay ours.
    uint64_t rt_status = aim->status;
    XrPosef rt_pose = aim->aimPose;
    float rt_qnorm2 = rt_pose.orientation.x * rt_pose.orientation.x + rt_pose.orientation.y * rt_pose.orientation.y +
                      rt_pose.orientation.z * rt_pose.orientation.z + rt_pose.orientation.w * rt_pose.orientation.w;
    // COMPUTED alone does not make the runtime ray valid. OpenXR poses also
    // require finite positions and unit quaternions (allow small roundoff).
    int rt_ok = G.aim_runtime > 0 &&
                (rt_status & (AIM_COMPUTED | AIM_VALID)) == (AIM_COMPUTED | AIM_VALID) &&
                isfinite(rt_pose.position.x) && isfinite(rt_pose.position.y) && isfinite(rt_pose.position.z) &&
                isfinite(rt_qnorm2) && fabsf(rt_qnorm2 - 1.0f) <= 0.01f;
    {
        static uint64_t last_rt[3] = { ~0ull, ~0ull, ~0ull };
        if ((rt_status & (AIM_COMPUTED | AIM_VALID)) != last_rt[hand]) {
            last_rt[hand] = rt_status & (AIM_COMPUTED | AIM_VALID);
            LOGI("aim: hand=%u runtime aim status=0x%llx p=(%.2f,%.2f,%.2f) q=(%.2f,%.2f,%.2f,%.2f) pinch=%.2f use=%d",
                 hand, (unsigned long long)rt_status, rt_pose.position.x, rt_pose.position.y, rt_pose.position.z,
                 rt_pose.orientation.x, rt_pose.orientation.y, rt_pose.orientation.z, rt_pose.orientation.w,
                 aim->pinchStrengthIndex, rt_ok);
        }
    }
    aim->status = 0;
    aim->pinchStrengthIndex = aim->pinchStrengthMiddle = aim->pinchStrengthRing = aim->pinchStrengthLittle = 0;

    static const int need[] = { J_WRIST, J_THUMB_TIP, J_INDEX_PROX, J_INDEX_TIP, J_MIDDLE_PROX, J_MIDDLE_TIP,
                                J_RING_PROX, J_RING_TIP, J_LITTLE_PROX, J_LITTLE_TIP };
    const XrHandJointLocationEXT* j = loc->jointLocations;
    int ok = loc->isActive && j && loc->jointCount >= 26;
    for (size_t i = 0; ok && i < sizeof need / sizeof need[0]; i++) ok = (j[need[i]].locationFlags & LOC_POS_VALID) != 0;
    int64_t t = now_ms();
    if (!ok) {
        memset(hs->pinching, 0, sizeof hs->pinching);
        hs->facing = 0; hs->menu_start_ms = 0; hs->init = 0;
        if (hs->menu_pressed) { hs->menu_pressed = 0; hs->menu_release_ms = t; }
        if (hs->last_mode != -1) { LOGI("aim: hand=%u invalid", hand); hs->last_mode = -1; }
        goto menu_out;
    }

    // Pinch strengths (tip distance minus both joint radii).
    static const int tips[4] = { J_INDEX_TIP, J_MIDDLE_TIP, J_RING_TIP, J_LITTLE_TIP };
    float* strength[4] = { &aim->pinchStrengthIndex, &aim->pinchStrengthMiddle, &aim->pinchStrengthRing, &aim->pinchStrengthLittle };
    float d_index = 1.0f;
    for (int f = 0; f < 4; f++) {
        float d = (vlen(vsub(jp(j, tips[f]), jp(j, J_THUMB_TIP))) - j[tips[f]].radius - j[J_THUMB_TIP].radius) * 100.0f;   // cm
        if (f == 0) d_index = d;
        // VD forwards pinchStrength to SteamVR as the trigger value (vd_hand_controller,
        // touch 0.1 / click 0.65): 0 from pinch_full_cm apart, quadratic up to 1 at pinch_on_cm.
        float s = (G.pinch_full_cm - d) / (G.pinch_full_cm - G.pinch_on_cm);
        s = s < 0 ? 0 : s > 1 ? 1 : s;
        *strength[f] = s * s;
        if (hs->pinching[f] ? d > G.pinch_off_cm : d < G.pinch_on_cm) hs->pinching[f] = !hs->pinching[f];
    }

    XrPosef head;
    int have_head = head_pose(session, li, &head);

    // Palm normal from WRIST + index/little proximal (PALM and metacarpals may
    // be invalid on PICO). In OpenXR hand space fingers run -Z, the back of the
    // hand is +Y and the right thumb is -X, so for the right hand
    // cross(index-wrist, little-wrist) points out of the palm; mirrored for left.
    V3 w = jp(j, J_WRIST);
    V3 palm_n = vnorm(vcross(vsub(jp(j, J_INDEX_PROX), w), vsub(jp(j, J_LITTLE_PROX), w)));
    if (hand == 1) palm_n = vscale(palm_n, -1.0f);
    V3 palm_c = vscale(vadd(w, jp(j, J_MIDDLE_PROX)), 0.5f);

    int facing = 0;
    float c = -1.0f;
    if (have_head) {
        c = vdot(palm_n, vnorm(vsub(head.position, palm_c)));
        float on = cosf(G.face_on_deg * 3.14159265f / 180.0f), off = cosf(G.face_off_deg * 3.14159265f / 180.0f);
        facing = hs->facing ? c > off : c > on;
    }
    hs->facing = facing;

    float fd[4] = { finger_dot(j, J_INDEX_PROX, J_INDEX_TIP), finger_dot(j, J_MIDDLE_PROX, J_MIDDLE_TIP),
                    finger_dot(j, J_RING_PROX, J_RING_TIP), finger_dot(j, J_LITTLE_PROX, J_LITTLE_TIP) };
    int curled = 0;
    for (int f = 0; f < 4; f++) curled += fd[f] < G.curl_dot;
    int others_ext = fd[1] > G.ext_dot && fd[2] > G.ext_dot && fd[3] > G.ext_dot;
    int menu_ext = fd[1] > G.menu_ext_dot && fd[2] > G.menu_ext_dot && fd[3] > G.menu_ext_dot;
    int menu_face = c > cosf(G.menu_face_deg * 3.14159265f / 180.0f);

    // Aim ray: blend of shoulder (estimated from the head) -> INDEX_PROXIMAL
    // and the hand's own wrist -> INDEX_PROXIMAL direction, plus a pitch offset.
    V3 origin = jp(j, J_INDEX_PROX), dir;
    V3 hand_dir = vnorm(vsub(origin, w));
    if (have_head && G.aim_shoulder > 0) {
        V3 right = qrot(head.orientation, v3(1, 0, 0));
        right.y = 0;
        right = vnorm(right);
        V3 shoulder = vadd(vsub(head.position, v3(0, 0.17f, 0)), vscale(right, hand == 2 ? 0.155f : -0.155f));
        V3 sd = vnorm(vsub(origin, shoulder));
        float k = G.aim_shoulder > 1 ? 1 : G.aim_shoulder;
        dir = vnorm(vadd(vscale(sd, k), vscale(hand_dir, 1 - k)));
    } else {
        dir = hand_dir;
    }
    if (G.aim_pitch_deg != 0 && vlen(dir) > 0.5f) {
        // rotate about the horizontal axis perpendicular to the ray
        V3 axis = vnorm(vcross(dir, v3(0, 1, 0)));
        if (vlen(axis) > 0.5f) {
            float a = G.aim_pitch_deg * 3.14159265f / 180.0f;
            dir = vnorm(vadd(vscale(dir, cosf(a)), vscale(vcross(axis, dir), sinf(a))));
        }
    }
    if (vlen(dir) < 0.5f) dir = v3(0, 0, -1);
    if (hs->init && G.aim_smooth > 0 && G.aim_smooth < 1) {
        float a = 1.0f - G.aim_smooth;
        hs->origin = vadd(vscale(hs->origin, G.aim_smooth), vscale(origin, a));
        hs->dir = vnorm(vadd(vscale(hs->dir, G.aim_smooth), vscale(dir, a)));
    } else {
        hs->origin = origin; hs->dir = dir; hs->init = 1;
    }
    // Prefer the runtime's (PICO system) ray; ours is the fallback.
    if (rt_ok) {
        aim->aimPose = rt_pose;
    } else {
        aim->aimPose.position = hs->origin;
        aim->aimPose.orientation = quat_look(hs->dir, v3(0, 1, 0));
    }

    // Ray diagnostics (~1 Hz per hand): pitch/yaw in degrees of the used ray,
    // of our own ray, of the runtime ray and of the head's -Z.
    static int64_t diag_ms[3];
    if (t - diag_ms[hand] >= 1000) {
        diag_ms[hand] = t;
        V3 used = qrot(aim->aimPose.orientation, v3(0, 0, -1));
        V3 rtd = qrot(rt_pose.orientation, v3(0, 0, -1));
        V3 hd = have_head ? qrot(head.orientation, v3(0, 0, -1)) : v3(0, 0, 0);
        const float R = 57.29578f;
        LOGI("aim: diag hand=%u src=%s used p/y=%.0f/%.0f ours p/y=%.0f/%.0f rt p/y=%.0f/%.0f rt_o=(%.2f,%.2f,%.2f) head p/y=%.0f/%.0f",
             hand, rt_ok ? "rt" : "ours", asinf(used.y) * R, atan2f(-used.x, -used.z) * R,
             asinf(hs->dir.y) * R, atan2f(-hs->dir.x, -hs->dir.z) * R,
             asinf(rtd.y) * R, atan2f(-rtd.x, -rtd.z) * R,
             rt_pose.position.x, rt_pose.position.y, rt_pose.position.z,
             asinf(hd.y) * R, atan2f(-hd.x, -hd.z) * R);
    }

    int mode;
    if (facing && curled >= 3) {
        mode = 3;   // fist toward the headset: leave it to the PICO system gesture
        aim->status = 0;
        aim->pinchStrengthIndex = aim->pinchStrengthMiddle = aim->pinchStrengthRing = aim->pinchStrengthLittle = 0;
        hs->menu_start_ms = 0;
        if (hs->menu_pressed) { hs->menu_pressed = 0; hs->menu_release_ms = t; }
    } else if (facing) {
        aim->status = AIM_COMPUTED | AIM_SYSTEM;
        aim->pinchStrengthIndex = aim->pinchStrengthMiddle = aim->pinchStrengthRing = aim->pinchStrengthLittle = 0;
        // Start needs the stricter menu thresholds; once holding, the normal
        // pinch hysteresis / facing keep it (so it does not flicker).
        float menu_hold_cm = G.menu_pinch_cm + 0.5f > G.pinch_off_cm ? G.menu_pinch_cm + 0.5f : G.pinch_off_cm;
        int menu_pose = hs->menu_start_ms || hs->menu_pressed
            ? d_index < menu_hold_cm && others_ext
            : d_index < G.menu_pinch_cm && menu_ext && menu_face;
        mode = menu_pose ? 2 : 1;
        if (menu_pose) {
            if (!hs->menu_pressed) {
                if (!hs->menu_start_ms) hs->menu_start_ms = t;
                if (t - hs->menu_start_ms >= (int64_t)G.menu_hold_ms && t - hs->menu_release_ms >= (int64_t)G.menu_cooldown_ms) {
                    hs->menu_pressed = 1;
                    LOGI("aim: hand=%u MENU_PRESSED", hand);
                }
            }
        } else {
            hs->menu_start_ms = 0;
            if (hs->menu_pressed) { hs->menu_pressed = 0; hs->menu_release_ms = t; LOGI("aim: hand=%u menu released", hand); }
        }
    } else {
        mode = 0;
        aim->status = AIM_COMPUTED | AIM_VALID;
        for (int f = 0; f < 4; f++) if (hs->pinching[f]) aim->status |= AIM_PINCH(f);
        hs->menu_start_ms = 0;
        if (hs->menu_pressed) { hs->menu_pressed = 0; hs->menu_release_ms = t; }
    }
    if (hand == 2) aim->status |= AIM_DOMINANT;
    if (mode != hs->last_mode) {
        LOGI("aim: hand=%u mode %d->%d head=%d face=%.0fdeg d_index=%.1fcm curled=%d ext=%d/%d fd=%.2f/%.2f/%.2f/%.2f",
             hand, hs->last_mode, mode, have_head, acosf(c < -1 ? -1 : c > 1 ? 1 : c) * 57.29578f, d_index, curled,
             others_ext, menu_ext, fd[0], fd[1], fd[2], fd[3]);
        hs->last_mode = mode;
    }

menu_out:
    // Meta semantics: MENU_PRESSED lives on the left hand; VD sends Enter to
    // the PC only for the left one. Either hand's gesture is reported there.
    g_menu_any = g_hs[1].menu_pressed || g_hs[2].menu_pressed;
    if (hand == 1 && g_menu_any) aim->status |= AIM_MENU;
}

int vdhs_hand_gipa_self(const char* name, PFN_xrVoidFunction* fn) {
    if (!strcmp(name, "xrGetHandMeshFB") && vdhs_hand_mesh_ready()) {
        *fn = (PFN_xrVoidFunction)w_get_hand_mesh;
        LOGI("gipa: serving %s from the layer", name);
        return 1;
    }
    return 0;
}

int vdhs_hand_gipa_wrap(const char* name, PFN_xrVoidFunction* fn) {
    PFN_xrVoidFunction* slot = NULL;
    PFN_xrVoidFunction w = NULL;
    if (!strcmp(name, "xrCreateHandTrackerEXT"))       { slot = (PFN_xrVoidFunction*)&g_next_create_ht;  w = (PFN_xrVoidFunction)w_create_ht; }
    else if (!strcmp(name, "xrDestroyHandTrackerEXT")) { slot = (PFN_xrVoidFunction*)&g_next_destroy_ht; w = (PFN_xrVoidFunction)w_destroy_ht; }
    else if (!strcmp(name, "xrLocateHandJointsEXT"))   { slot = (PFN_xrVoidFunction*)&g_next_locate_hj;  w = (PFN_xrVoidFunction)w_locate_hj; }
    if (!slot) return 0;
    *slot = *fn;
    *fn = w;
    LOGI("gipa: wrapped %s", name);
    return 1;
}

// ---------------------------------------------------------------- managed injection (plan A)
typedef void* MP;
static struct {
    MP (*image_loaded)(const char*);
    void (*assembly_foreach)(void (*)(MP, MP), MP);
    MP (*assembly_get_image)(MP);
    const char* (*image_get_name)(MP);
    MP (*class_from_name)(MP, const char*, const char*);
    MP (*class_get_field_from_name)(MP, const char*);
    MP (*class_vtable)(MP, MP);
    MP (*domain_get)(void);
    void (*field_static_get_value)(MP, MP, void*);
    MP (*object_get_class)(MP);
    MP (*class_get_method_from_name)(MP, const char*, int);
    MP (*runtime_invoke)(MP, MP, void**, MP*);
    void* (*object_unbox)(MP);
    MP (*object_to_string)(MP, MP*);
    char* (*string_to_utf8)(MP);
    MP (*get_root_domain)(void);
    MP (*thread_attach)(MP);
    MP (*string_new)(MP, const char*);
    void (*field_static_set_value)(MP, MP, void*);
    void (*mono_free)(void*);
    MP (*class_get_parent)(MP);
    MP (*class_get_property_from_name)(MP, const char*);
    MP (*property_get_get_method)(MP);
    MP (*property_get_set_method)(MP);
} M;
// Optional (pt_follow_settings only): a missing symbol must not disable the
// permission / extension fixes that depend on M.
static struct {
    void (*field_get_value)(MP, MP, void*);
    uintptr_t (*array_length)(MP);
    char* (*array_addr_with_size)(MP, int, uintptr_t);
} MX;

static int mono_resolve(void) {
    static const char* const names[] = {
        "mono_image_loaded", "mono_assembly_foreach", "mono_assembly_get_image", "mono_image_get_name",
        "mono_class_from_name", "mono_class_get_field_from_name", "mono_class_vtable", "mono_domain_get",
        "mono_field_static_get_value", "mono_object_get_class", "mono_class_get_method_from_name",
        "mono_runtime_invoke", "mono_object_unbox", "mono_object_to_string", "mono_string_to_utf8",
        "mono_get_root_domain", "mono_thread_attach", "mono_string_new", "mono_field_static_set_value", "mono_free",
        "mono_class_get_parent", "mono_class_get_property_from_name", "mono_property_get_get_method",
        "mono_property_get_set_method",
    };
    _Static_assert(sizeof names / sizeof names[0] == sizeof M / sizeof(void*), "M slots vs names");
    static int resolved;
    if (resolved) return 1;
    void** slots = (void**)&M;
    int ok = 1;
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        slots[i] = vdhs_resolve_mapped_symbol("libmonosgen-2.0.so", names[i]);
        if (!slots[i]) { LOGE("inject: %s not found", names[i]); ok = 0; }
    }
    if (ok) resolved = 1;
    return ok;
}

static MP g_found_image;
static void find_image_cb(MP assembly, MP user) {
    if (g_found_image) return;
    MP img = M.assembly_get_image(assembly);
    const char* nm = img ? M.image_get_name(img) : NULL;
    if (nm && !strcmp(nm, (const char*)user)) g_found_image = img;
}
static MP find_image(const char* name) {
    MP img = M.image_loaded(name);
    if (!img) { g_found_image = NULL; M.assembly_foreach(find_image_cb, (MP)name); img = g_found_image; }
    return img;
}

static void log_exc(const char* what, MP exc) {
    MP inner = NULL;
    MP s = M.object_to_string(exc, &inner);
    char* msg = (s && !inner) ? M.string_to_utf8(s) : NULL;
    LOGE("inject: %s raised: %s", what, msg ? msg : "(unprintable)");
}

static int hashset_count(MP set, MP get_count) {
    MP exc = NULL;
    MP boxed = M.runtime_invoke(get_count, set, NULL, &exc);
    if (exc || !boxed) return -1;
    return *(int32_t*)M.object_unbox(boxed);
}

static char* obj_to_utf8(MP obj) {
    return obj ? M.string_to_utf8(obj) : NULL;
}

// Static auto-property value/setter via its property accessors (works for
// both static and instance properties; `self` NULL for static).
static MP prop_get(MP klass, MP self, const char* name) {
    for (MP k = klass; k; k = M.class_get_parent(k)) {
        MP p = M.class_get_property_from_name(k, name);
        MP g = p ? M.property_get_get_method(p) : NULL;
        if (!g) continue;
        MP exc = NULL;
        MP v = M.runtime_invoke(g, self, NULL, &exc);
        if (exc) { log_exc(name, exc); return NULL; }
        return v;
    }
    return NULL;
}
static int prop_set(MP klass, MP self, const char* name, void* arg) {
    for (MP k = klass; k; k = M.class_get_parent(k)) {
        MP p = M.class_get_property_from_name(k, name);
        MP s = p ? M.property_get_set_method(p) : NULL;
        if (!s) continue;
        void* argv[1] = { arg };
        MP exc = NULL;
        M.runtime_invoke(s, self, argv, &exc);
        if (exc) { log_exc(name, exc); return -1; }
        return 0;
    }
    return -2;
}

// VD 1.34.22 on PICO asks for permissions that do not exist there
// (VrApp.cs:176 FACE = com.oculus.permission.FACE_TRACKING; InputTab.cs:84
// EYE = android.permission.EYE_TRACKING_FINE). Requests fail at once, so
// "Forward tracking data" (StreamingTab.cs:431/1196) and "Eye gaze"
// (InputTab.cs:151/511) switch themselves off. Point them at the granted PICO
// permission. Must run after DeterminePlatform (VrApp.OnCreate) and before
// ControlPanel.LoadContent builds the tabs; xrCreateInstance is in between.
int vdhs_hand_fix_permissions(void) {
    if (!mono_resolve()) return -1;
    static const char kPicoEye[] = "com.picovr.permission.EYE_TRACKING";
    int rc = 0;

    // 1) DynamicSettings.Default.FaceTrackingPermission (instance auto-property).
    MP img = find_image("VirtualDesktop.Mobile");
    MP ds = img ? M.class_from_name(img, "VirtualDesktop.Mobile", "DynamicSettings") : NULL;
    MP def = ds ? prop_get(ds, NULL, "Default") : NULL;
    if (!def) { LOGE("perm: DynamicSettings.Default not found (img=%p cls=%p)", img, ds); rc = -2; }
    else {
        char* face = obj_to_utf8(prop_get(ds, def, "FaceTrackingPermission"));
        char* eye = obj_to_utf8(prop_get(ds, def, "EyeTrackingPermission"));
        LOGI("perm: DynamicSettings eye=%s face=%s", eye ? eye : "(null)", face ? face : "(null)");
        if (face && strcmp(face, "com.oculus.permission.FACE_TRACKING") == 0 && eye && strstr(eye, "picovr")) {
            // Face tracking on PICO has no Android permission VD knows about;
            // use the granted PICO eye-tracking one so the gate passes.
            MP s = M.string_new(M.domain_get(), kPicoEye);
            int r = prop_set(ds, def, "FaceTrackingPermission", s);
            char* now = obj_to_utf8(prop_get(ds, def, "FaceTrackingPermission"));
            LOGI("perm: FaceTrackingPermission -> %s (r=%d)", now ? now : "(null)", r);
            if (r) rc = -3;
            if (now) M.mono_free(now);
        }
        if (face) M.mono_free(face);
        if (eye) M.mono_free(eye);
    }

    // 2) InputTab._eyeTrackingPermission is assigned in InputTab's constructor
    //    (InputTab.cs:84), so it is fixed later from the permission probe hook
    //    below; here we only confirm the class exists.
    MP it = img ? M.class_from_name(img, "VirtualDesktop.Mobile", "InputTab") : NULL;
    LOGI("perm: InputTab class=%p", it);
    return rc;
}

// UserSettings.Default.PreferEyeGazeInput as saved, captured at xrCreateInstance
// (before ControlPanel.LoadContent). InputTab's constructor checks the wrong
// permission name before we can fix it (InputTab.cs:84 then :151) and clears
// the setting while Game has not subscribed to the change event yet
// (Game.cs:1416 vs :1426): the checkbox shows off, the HMD keeps gaze on.
static int g_saved_eye_gaze = -1;
static MP user_settings(void) {
    MP img = find_image("VirtualDesktop.Mobile");
    MP us = img ? M.class_from_name(img, "VirtualDesktop.Mobile", "UserSettings") : NULL;
    return us ? prop_get(us, NULL, "Default") : NULL;
}
void vdhs_hand_capture_settings(void) {
    if (!mono_resolve()) return;
    MP def = user_settings();
    MP v = def ? prop_get(M.object_get_class(def), def, "PreferEyeGazeInput") : NULL;
    if (v) g_saved_eye_gaze = *(uint8_t*)M.object_unbox(v);
    LOGI("perm: saved PreferEyeGazeInput=%d", g_saved_eye_gaze);
}
// After InputTab's permission name is fixed: put the saved value back through
// the property setter, so the checkbox, the save file and the HMD agree.
static void restore_eye_gaze(void) {
    if (g_saved_eye_gaze != 1) return;
    MP def = user_settings();
    if (!def) return;
    MP k = M.object_get_class(def);
    MP v = prop_get(k, def, "PreferEyeGazeInput");
    if (!v || *(uint8_t*)M.object_unbox(v)) { g_saved_eye_gaze = 0; return; }
    uint8_t on = 1;
    int r = prop_set(k, def, "PreferEyeGazeInput", &on);
    LOGI("perm: PreferEyeGazeInput restored to 1 (r=%d)", r);
    g_saved_eye_gaze = 0;
}

// ---------------------------------------------------------------- pt_hole gate
// pt_follow_settings=1: the alpha hole follows VD's own hand passthrough
// logic for SteamVR (PassthroughPortals.Update:109-123 / EndDraw:264-272):
//   Scene.LoadedEnvironment != Passthrough,
//   SharedStreamerSettings.StreamingSource == VR,
//   DynamicSettings.CanUseVRPassthrough, UserSettings.VRPassthroughHands,
//   Hmd.ActiveControllerType == Hand,
//   min(VRPassthroughOpacity, Scene.Opacity) > 0 (VD draws nothing at 0).
// The hole only ever goes into OpenXRHMD._vrChains / _vrExtrapolatedChains
// (the SteamVR stream, OpenXRHMD.cs:933/972), never into the desktop UI
// projection layer. Outside SteamVR VD's Input -> Hand passthrough owns the
// hands and the layer is left alone.
enum { kStreamingVR = 1, kEnvPassthrough = 1, kCtrlHand = 2 };

static int boxed_u8(MP v, int* out) { if (!v) return 0; *out = *(uint8_t*)M.object_unbox(v); return 1; }
static int boxed_i32(MP v, int* out) { if (!v) return 0; *out = *(int32_t*)M.object_unbox(v); return 1; }
static int boxed_f32(MP v, float* out) { if (!v) return 0; *out = *(float*)M.object_unbox(v); return 1; }
static MP obj_prop(MP self, const char* name) { return self ? prop_get(M.object_get_class(self), self, name) : NULL; }

static int mx_resolve(void) {
    static int state;   // 0 untried, 1 ok, -1 missing
    if (state) return state > 0;
    MX.field_get_value = (void (*)(MP, MP, void*))vdhs_resolve_mapped_symbol("libmonosgen-2.0.so", "mono_field_get_value");
    MX.array_length = (uintptr_t (*)(MP))vdhs_resolve_mapped_symbol("libmonosgen-2.0.so", "mono_array_length");
    MX.array_addr_with_size = (char* (*)(MP, int, uintptr_t))vdhs_resolve_mapped_symbol("libmonosgen-2.0.so",
                                                                                         "mono_array_addr_with_size");
    state = (MX.field_get_value && MX.array_length && MX.array_addr_with_size) ? 1 : -1;
    if (state < 0) LOGE("pt: gate mono field/array API missing");
    return state > 0;
}

// Instance field of `self` or of one of its base classes (reference type).
static MP obj_field_ref(MP self, const char* name) {
    for (MP k = M.object_get_class(self); k; k = M.class_get_parent(k)) {
        MP f = M.class_get_field_from_name(k, name);
        if (!f) continue;
        MP v = NULL;
        MX.field_get_value(self, f, &v);
        return v;
    }
    return NULL;
}

// Swapchain handles of a TextureSwapChain[] (TextureSwapChain.SwapChain).
static int chain_handles(MP arr, uint64_t* out, int n, int max) {
    if (!arr) return n;
    uintptr_t len = MX.array_length(arr);
    for (uintptr_t i = 0; i < len && n < max; i++) {
        MP tc = *(MP*)MX.array_addr_with_size(arr, (int)sizeof(MP), i);
        int64_t h = 0;
        MP v = obj_prop(tc, "SwapChain");
        if (v) h = *(int64_t*)M.object_unbox(v);
        if (h) out[n++] = (uint64_t)h;
    }
    return n;
}

// 1 = draw, 0 = VD would not show hand passthrough, -1 = could not read.
static int pt_gate_read(uint64_t* chains, int max, int* nchains, char* why, size_t cap) {
    *nchains = 0;
    if (!mono_resolve() || !mx_resolve()) { snprintf(why, cap, "mono api"); return -1; }
    MP mob = find_image("VirtualDesktop.Mobile");
    MP shr = find_image("VirtualDesktop.Mobile.Shared");
    if (!mob || !shr) { snprintf(why, cap, "image mob=%p shared=%p", mob, shr); return -1; }

    MP k = M.class_from_name(shr, "VirtualDesktop.Mobile", "SharedStreamerSettings");
    int src = -1;
    if (!k || !boxed_u8(obj_prop(prop_get(k, NULL, "Default"), "StreamingSource"), &src)) {
        snprintf(why, cap, "StreamingSource unreadable"); return -1;
    }
    if (src != kStreamingVR) { snprintf(why, cap, "StreamingSource=%d (not VR)", src); return 0; }

    k = M.class_from_name(mob, "VirtualDesktop.Mobile", "DynamicSettings");
    int can = -1;
    if (!k || !boxed_u8(obj_prop(prop_get(k, NULL, "Default"), "CanUseVRPassthrough"), &can)) {
        snprintf(why, cap, "CanUseVRPassthrough unreadable"); return -1;
    }
    if (!can) { snprintf(why, cap, "CanUseVRPassthrough=0"); return 0; }

    MP us = user_settings();
    int hands = -1;
    float vr_op = 1.0f;
    if (!boxed_u8(obj_prop(us, "VRPassthroughHands"), &hands) || !boxed_f32(obj_prop(us, "VRPassthroughOpacity"), &vr_op)) {
        snprintf(why, cap, "UserSettings unreadable"); return -1;
    }
    if (!hands) { snprintf(why, cap, "VRPassthroughHands=0"); return 0; }

    // Game instance: InputManager._game (InputManager.cs:16, set in VrApp.cs:100).
    k = M.class_from_name(mob, "VirtualDesktop.Mobile", "InputManager");
    MP f = k ? M.class_get_field_from_name(k, "_game") : NULL;
    MP vt = k ? M.class_vtable(M.domain_get(), k) : NULL;
    MP game = NULL;
    if (f && vt) M.field_static_get_value(vt, f, &game);
    if (!game) { snprintf(why, cap, "Game unreadable"); return -1; }

    MP scene = obj_prop(game, "Scene");
    int env = -1;
    float sc_op = 1.0f;
    if (!boxed_i32(obj_prop(scene, "LoadedEnvironment"), &env) || !boxed_f32(obj_prop(scene, "Opacity"), &sc_op)) {
        snprintf(why, cap, "Scene unreadable"); return -1;
    }
    if (env == kEnvPassthrough) { snprintf(why, cap, "environment=Passthrough"); return 0; }
    float op = vr_op < sc_op ? vr_op : sc_op;
    if (!(op > 0.01f)) { snprintf(why, cap, "opacity=%.2f", op); return 0; }

    MP hmd = obj_prop(game, "Hmd");
    int ctl = -1;
    if (!boxed_i32(obj_prop(hmd, "ActiveControllerType"), &ctl)) { snprintf(why, cap, "Hmd unreadable"); return -1; }
    if (ctl != kCtrlHand) { snprintf(why, cap, "ActiveControllerType=%d (not Hand)", ctl); return 0; }

    int n = chain_handles(obj_field_ref(hmd, "_vrChains"), chains, 0, max);
    n = chain_handles(obj_field_ref(hmd, "_vrExtrapolatedChains"), chains, n, max);
    *nchains = n;
    if (!n) { snprintf(why, cap, "no VR swapchains"); return 0; }
    snprintf(why, cap, "SteamVR hands opacity=%.2f chains=%d", op, n);
    return 1;
}

// Called on VD's render thread (inside the xrEndFrame P/Invoke, Mono-attached).
// Re-read about once per second; the result is cached in between.
// `hint_on`: the current frame suggests the gate should be on (VD submitted a
// projected passthrough layer); a cached "off" is then re-read after 200 ms
// instead of waiting for the 1 s period, so the hole follows VD's portals.
int vdhs_hand_pt_gate(uint64_t* chains, int max, int* nchains, int hint_on) {
    static int64_t next_ms, last_read_ms;
    static int state = -2;
    static uint64_t cached[8];
    static int ncached;
    int64_t t = now_ms();
    if (t >= next_ms || (hint_on && state == 0 && t - last_read_ms >= 200)) {
        last_read_ms = t;
        char why[96];
        uint64_t c[8] = { 0 };
        int n = 0;
        int s = pt_gate_read(c, 8, &n, why, sizeof why);
        next_ms = t + (s < 0 ? 5000 : 1000);   // back off when Mono is not readable
        if (s != state) LOGI("pt: gate %s (%s)", s > 0 ? "ON" : s == 0 ? "off" : "unknown->off", why);
        state = s;
        ncached = n;
        memcpy(cached, c, sizeof c);
    }
    int n = ncached < max ? ncached : max;
    if (chains && n > 0) memcpy(chains, cached, (size_t)n * sizeof *chains);
    if (nchains) *nchains = n;
    return state > 0;
}

// Re-point InputTab._eyeTrackingPermission (private static) at the PICO
// permission. Idempotent; called whenever the layer gets a chance on VD's thread.
int vdhs_hand_fix_inputtab(void) {
    if (!mono_resolve()) return -1;
    MP img = find_image("VirtualDesktop.Mobile");
    MP it = img ? M.class_from_name(img, "VirtualDesktop.Mobile", "InputTab") : NULL;
    MP f = it ? M.class_get_field_from_name(it, "_eyeTrackingPermission") : NULL;
    MP vt = it ? M.class_vtable(M.domain_get(), it) : NULL;
    if (!f || !vt) return -2;
    MP cur = NULL;
    M.field_static_get_value(vt, f, &cur);
    char* s = obj_to_utf8(cur);
    int changed = 0;
    if (s && !strcmp(s, "android.permission.EYE_TRACKING_FINE")) {
        MP ns = M.string_new(M.domain_get(), "com.picovr.permission.EYE_TRACKING");
        M.field_static_set_value(vt, f, ns);
        changed = 1;
        LOGI("perm: InputTab._eyeTrackingPermission %s -> com.picovr.permission.EYE_TRACKING", s);
        restore_eye_gaze();
    }
    if (s) M.mono_free(s);
    return changed;
}

// Runs on VD's own thread inside the P/Invoke of xrCreateInstance, i.e. after
// XR.cs:1159 assigned _supportedExtensions and before any hand-tracking check.
int vdhs_hand_inject_managed(const int32_t* values, int count) {
    if (!mono_resolve()) return -1;
    MP img = find_image("Xenko.OpenXR");
    if (!img) { LOGE("inject: Xenko.OpenXR image not found"); return -2; }
    MP xr = M.class_from_name(img, "Xenko.OpenXR", "XR");
    MP field = xr ? M.class_get_field_from_name(xr, "_supportedExtensions") : NULL;
    MP domain = M.domain_get();
    MP vt = (xr && domain) ? M.class_vtable(domain, xr) : NULL;
    if (!field || !vt) { LOGE("inject: XR=%p field=%p vtable=%p domain=%p", xr, field, vt, domain); return -3; }

    MP set = NULL;
    M.field_static_get_value(vt, field, &set);
    if (!set) { LOGE("inject: XR._supportedExtensions is null"); return -4; }
    MP k = M.object_get_class(set);
    MP add = M.class_get_method_from_name(k, "Add", 1);
    MP cnt = M.class_get_method_from_name(k, "get_Count", 0);
    if (!add || !cnt) { LOGE("inject: HashSet Add=%p get_Count=%p", add, cnt); return -5; }

    int before = hashset_count(set, cnt);
    int rc = 0;
    for (int i = 0; i < count; i++) {
        int32_t v = values[i];
        void* argv[1] = { &v };
        MP exc = NULL;
        MP boxed = M.runtime_invoke(add, set, argv, &exc);
        if (exc) { log_exc("HashSet.Add", exc); rc = -6; break; }
        LOGI("inject: _supportedExtensions.Add(%d) -> %d", v, boxed ? *(uint8_t*)M.object_unbox(boxed) : -1);
    }
    LOGI("inject: _supportedExtensions count %d -> %d rc=%d", before, hashset_count(set, cnt), rc);
    return rc;
}
