// Read PICO's battery input action and fill the two bytes VD already sends.
// The installed APK, network packet layout and PC driver stay unchanged.
#define _GNU_SOURCE
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <android/log.h>

#include "shadowhook.h"
#include "vdhs_battery.h"
#include "vdhs_layer.h"
#include "vdhs_sym.h"

#define TAG "vdhs-battery"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

typedef uint64_t XrHandle;
typedef int32_t XrResult;
typedef struct {
    uint32_t type; const void* next;
    char actionName[64]; int32_t actionType;
    uint32_t countSubactionPaths; const XrHandle* subactionPaths;
    char localizedActionName[128];
} ActionCreateInfo;
typedef struct { XrHandle action, binding; } SuggestedBinding;
typedef struct {
    uint32_t type; const void* next; XrHandle interactionProfile;
    uint32_t countSuggestedBindings; const SuggestedBinding* suggestedBindings;
} SuggestedBindings;
typedef struct {
    uint32_t type; const void* next; XrHandle action, subactionPath;
} ActionStateGetInfo;
typedef struct {
    uint32_t type; void* next; float currentState;
    uint32_t changedSinceLastSync; int64_t lastChangeTime; uint32_t isActive;
} ActionStateFloat;
_Static_assert(sizeof(ActionCreateInfo) == 224, "OpenXR arm64 action-create ABI");
_Static_assert(sizeof(SuggestedBindings) == 40, "OpenXR arm64 suggested-bindings ABI");
_Static_assert(sizeof(ActionStateGetInfo) == 32, "OpenXR arm64 state-query ABI");
_Static_assert(sizeof(ActionStateFloat) == 40, "OpenXR arm64 float-state ABI");
typedef XrResult (*CreateAction)(XrHandle, const ActionCreateInfo*, XrHandle*);
typedef XrResult (*StringToPath)(XrHandle, const char*, XrHandle*);
typedef XrResult (*GetFloat)(XrHandle, const ActionStateGetInfo*, ActionStateFloat*);
typedef XrResult (*Suggest)(XrHandle, const SuggestedBindings*);
typedef XrResult (*Sync)(XrHandle, const void*);

static Suggest g_suggest;
static Sync g_sync;
static CreateAction g_create_action;
static GetFloat g_get_float;
static XrHandle g_instance, g_action, g_profile, g_hands[2], g_sources[2], g_session;
static int g_hooks_ready, g_bound;
static int g_writer_status = -2;
static int g_percent[2];
static int64_t g_next_poll;

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void vdhs_battery_begin_instance(uint64_t instance, vdhs_xr_get_proc get_proc) {
    g_instance = 0;
    g_action = g_profile = g_session = 0;
    g_bound = 0;
    g_writer_status = -2;
    // This bridge reserves -1 for unavailable. It is not a PICO sentinel;
    // the PICO source is an unsigned byte and uses Action isActive instead.
    g_percent[0] = g_percent[1] = -1;
    g_next_poll = 0;
    if (!g_hooks_ready) return;
    StringToPath string_to_path = NULL;
    if (get_proc(instance, "xrStringToPath", (vdhs_xr_function*)&string_to_path) != 0 ||
        get_proc(instance, "xrCreateAction", (vdhs_xr_function*)&g_create_action) != 0 ||
        get_proc(instance, "xrGetActionStateFloat", (vdhs_xr_function*)&g_get_float) != 0 ||
        !string_to_path || !g_create_action || !g_get_float) {
        LOGE("core input functions unavailable");
        return;
    }
    // VD 1.34.22 uses the Neo3 profile on PICO 4 as well as Neo3.
    const char* paths[] = {
        "/interaction_profiles/pico/neo3_controller",
        "/user/hand/left", "/user/hand/right",
        "/user/hand/left/input/battery/value", "/user/hand/right/input/battery/value",
    };
    XrHandle* targets[] = { &g_profile, &g_hands[0], &g_hands[1], &g_sources[0], &g_sources[1] };
    for (unsigned i = 0; i < sizeof paths / sizeof paths[0]; i++) {
        XrResult r = string_to_path(instance, paths[i], targets[i]);
        if (r != 0) { LOGE("path %s failed: %d", paths[i], r); return; }
    }
    g_instance = instance;
}

void vdhs_battery_on_grip_action(uint64_t action_set) {
    if (!g_instance || g_action) return;
    ActionCreateInfo info = {
        .type = 29, .actionName = "vdhs_battery", .actionType = 2,
        .countSubactionPaths = 2, .subactionPaths = g_hands,
        .localizedActionName = "PICO controller battery",
    };
    XrResult r = g_create_action(action_set, &info, &g_action);
    if (r != 0) g_action = 0;
    LOGI("create battery action r=%d action=0x%llx", r, (unsigned long long)g_action);
}

static XrResult hk_suggest(XrHandle instance, const SuggestedBindings* info) {
    if (!vdhs_layer_active() || !g_hooks_ready || !g_action ||
        instance != g_instance || !info || info->interactionProfile != g_profile)
        return g_suggest(instance, info);
    uint32_t n = info->countSuggestedBindings;
    SuggestedBinding bindings[n + 2];
    memcpy(bindings, info->suggestedBindings, n * sizeof *bindings);
    bindings[n] = (SuggestedBinding){ g_action, g_sources[0] };
    bindings[n + 1] = (SuggestedBinding){ g_action, g_sources[1] };
    SuggestedBindings copy = *info;
    copy.countSuggestedBindings = n + 2;
    copy.suggestedBindings = bindings;
    XrResult r = g_suggest(instance, &copy);
    g_bound = r == 0;
    LOGI("suggest battery bindings r=%d", r);
    // VD CheckError() throws on a rejected profile: preserve its original input
    // bindings if this firmware does not accept the private battery source.
    return g_bound ? r : g_suggest(instance, info);
}

static XrResult hk_sync(XrHandle session, const void* info) {
    XrResult r = g_sync(session, info);
    if (!vdhs_layer_active() || !g_hooks_ready || !g_bound) return r;
    int64_t now = now_ms();
    if (session != g_session || r != 0 || now >= g_next_poll) {
        int percent[2] = { -1, -1 };
        float raw[2] = { 0, 0 };
        uint32_t valid[2] = { 0, 0 };
        XrResult results[2] = { r, r };
        if (r == 0) {
            for (int hand = 0; hand < 2; hand++) {
                ActionStateGetInfo query = { .type = 58, .action = g_action, .subactionPath = g_hands[hand] };
                ActionStateFloat state = { .type = 24 };
                XrResult result = g_get_float(session, &query, &state);
                results[hand] = result;
                raw[hand] = state.currentState;
                valid[hand] = state.isActive;
                // PICO forwards the raw battery byte as float. Its Shell uses
                // five steps, not a normalized 0..1 value. Keep 0% distinct
                // from inactive/failed or unrecognized (e.g. 255) samples.
                if (result == 0 && state.isActive && state.currentState >= 0.0f && state.currentState <= 5.0f)
                    percent[hand] = (int)(state.currentState * 20.0f + 0.5f);
            }
        }
        if (percent[0] != g_percent[0] || percent[1] != g_percent[1] || session != g_session)
            LOGI("sample raw=%.3f/%.3f active=%u/%u result=%d/%d percent=%d/%d sync=%d",
                 raw[0], raw[1], valid[0], valid[1], results[0], results[1], percent[0], percent[1], r);
        g_percent[0] = percent[0];
        g_percent[1] = percent[1];
        g_session = session;
        g_next_poll = now + 1000;
    }
    // Refresh just the two fields every sync: VD may have cleared DeviceState
    // while disconnected. Do not copy/replace a frame containing live poses.
    int written = vdhs_managed_set_controller_battery(g_percent[0], g_percent[1]);
    if (written != g_writer_status) {
        LOGI("frame battery fields status=%d (1=written, 0=waiting, -1=unavailable)", written);
        g_writer_status = written;
    }
    return r;
}

int vdhs_battery_install(void) {
    const char* loader = "/lib/arm64/libopenxr_loader.so";
    void* suggest = vdhs_resolve_mapped_symbol(loader, "xrSuggestInteractionProfileBindings");
    void* sync = vdhs_resolve_mapped_symbol(loader, "xrSyncActions");
    if (!suggest || !sync) { LOGE("loader input exports unavailable"); return -1; }
    void* first = shadowhook_hook_func_addr(suggest, (void*)hk_suggest, (void**)&g_suggest);
    if (!first) { LOGE("suggest hook failed: %d", shadowhook_get_errno()); return -1; }
    void* second = shadowhook_hook_func_addr(sync, (void*)hk_sync, (void**)&g_sync);
    if (!second) {
        LOGE("sync hook failed: %d", shadowhook_get_errno());
        shadowhook_unhook(first);
        return -1;
    }
    g_hooks_ready = 1;
    return 0;
}
