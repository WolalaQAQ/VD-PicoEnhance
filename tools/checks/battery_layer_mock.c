// Only the selected layer entry points are production code. Unrelated hooks,
// runtime marking, hand tracking and allocation below are offline dependencies.
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <android/log.h>
#include "shadowhook.h"
#include "../../mod/native/vdhs_battery.h"
#include "../../mod/native/vdhs_layer.h"

typedef int pthread_mutex_t;
#define PTHREAD_MUTEX_INITIALIZER 0
static int pthread_mutex_lock(pthread_mutex_t* mutex) { ++*mutex; return 0; }
static int pthread_mutex_unlock(pthread_mutex_t* mutex) { --*mutex; return 0; }

// These types, globals and constants also come from vdhs_layer.c, unchanged.
#include "layer_battery_declarations.h"

typedef uint64_t XrHandle;

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "layer-mock", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "layer-mock", __VA_ARGS__)
#define TS() ((long long)(0 - g_t0))
static XrAction g_gaze_action;
static void* g_orig_end_frame;
static void* g_orig_poll_event;
static void* g_orig_create_action_space;
static void* g_orig_locate_space;
static int create_result;
static unsigned char allocation[512];

XrHandle mock_open_instance(void);
void* mock_loader_entry(const char* name);
void* vdhs_resolve_mapped_symbol(const char* library, const char* name);

static void* calloc(size_t count, size_t size) {
    if (count * size > sizeof allocation) return NULL;
    return memset(allocation, 0, count * size);
}
static void free(void* memory) { (void)memory; }
static void try_mark(const char* where) { (void)where; g_marked = 1; }
void* vdhs_runtime_gipa(void) { return (void*)1; }
void vdhs_runtime_reset(void) {}
void vdhs_caps_begin_instance(uint32_t count, const char* const* names) { (void)count; (void)names; }
int vdhs_hand_mesh_ready(void) { return 0; }
int vdhs_hand_inject_managed(const int32_t* values, int count) { (void)values; (void)count; return 0; }
void vdhs_hand_capture_settings(void) {}
int vdhs_pt_install(uintptr_t base) { (void)base; return 0; }

// install_hooks needs these addresses, but these unrelated entry points are
// never called by this fixture. Every original has a distinct address.
#define UNUSED_ENTRY(name) static void name(void) {}
UNUSED_ENTRY(hk_enum)
UNUSED_ENTRY(hk_gipa)
UNUSED_ENTRY(hk_get_pose)
UNUSED_ENTRY(hk_get_system_props)
UNUSED_ENTRY(hk_end_frame)
UNUSED_ENTRY(hk_poll_event)
UNUSED_ENTRY(hk_create_action_space)
UNUSED_ENTRY(hk_locate_space)
UNUSED_ENTRY(original_enum)
UNUSED_ENTRY(original_get_pose)
UNUSED_ENTRY(original_get_system_props)
UNUSED_ENTRY(original_end_frame)
UNUSED_ENTRY(original_poll_event)
UNUSED_ENTRY(original_create_action_space)
UNUSED_ENTRY(original_locate_space)
#undef UNUSED_ENTRY

static XrResult original_create_instance(const XrInstanceCreateInfo* info, XrInstance* out) {
    if (!info) return -1;
    *out = create_result ? 0 : mock_open_instance();
    return create_result;
}

// is_layer_ext, hk_create_instance, hk_create_action, vdhs_layer_active and
// install_hooks are extracted by function boundaries; no mirrored hook logic.
#include "layer_battery_functions.h"

void* battery_layer_resolve(const char* name) {
    if (!strcmp(name, "xrCreateInstance")) return (void*)original_create_instance;
    const struct { const char* name; void (*function)(void); } exports[] = {
        { "xrEnumerateInstanceExtensionProperties", original_enum },
        { "xrGetActionStatePose", original_get_pose },
        { "xrGetSystemProperties", original_get_system_props },
        { "xrEndFrame", original_end_frame },
        { "xrPollEvent", original_poll_event },
        { "xrCreateActionSpace", original_create_action_space },
        { "xrLocateSpace", original_locate_space },
    };
    for (size_t i = 0; i < sizeof exports / sizeof exports[0]; i++)
        if (!strcmp(name, exports[i].name)) return (void*)exports[i].function;
    return NULL;
}
void battery_layer_reset(void) {
    g_hooks_ok = g_layer_started = g_marked = 0;
    g_instance = g_gaze_action = 0;
    g_grip_n = g_lock = 0;
    g_t0 = 0;
    create_result = 0;
    // This fixture leaves extension enumeration itself outside its scope.
    (void)g_advertise_layer_exts;
}
int battery_layer_install(void) { return install_hooks(0); }
XrResult battery_layer_create_instance(int result, XrHandle* out) {
    create_result = result;
    XrInstanceCreateInfo info = { .type = 3 };
    PFN_create_instance entry = (PFN_create_instance)mock_loader_entry("xrCreateInstance");
    return entry(&info, out);
}
int battery_layer_observed(XrHandle grip, XrHandle eye) {
    return g_grip_n == 1 && g_grip[0] == grip && g_gaze_action == eye && g_lock == 0;
}
