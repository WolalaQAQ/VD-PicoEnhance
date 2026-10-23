// Execute the production battery module against an instance-owned offline loader.
#include "../../mod/native/vdhs_battery.c"

int _fltused; // MSVC-ABI floating-point marker; no C runtime is needed.

#define CHECK(test) do { if (!(test)) return __LINE__; } while (0)
enum { INVALID_HANDLE = -12, NOT_ATTACHED = -46, NOT_SYNCED = -1001 };
typedef struct { XrHandle handle, paths[8]; int alive; } MockInstance;
typedef struct { XrHandle handle, instance; int attached; } MockSet;
typedef struct {
    XrHandle handle, instance, set;
    int battery, bound;
} MockAction;
typedef struct {
    XrHandle handle, instance, attached[2], synced[2];
    uint32_t attached_count, synced_count;
} MockSession;
typedef struct {
    uint32_t type; const void* next;
    uint32_t count; const XrHandle* sets;
} AttachInfo;
typedef struct { XrHandle set, subaction; } ActiveSet;
typedef struct {
    uint32_t type; const void* next;
    uint32_t count; const ActiveSet* sets;
} SyncInfo;
typedef struct { void* address; void* replacement; int installed; } MockHook;
typedef struct { XrHandle instance, set, eye_set, original[2], session; } Rig;

static MockInstance instances[4];
static MockSet sets[8];
static MockAction actions[32];
static MockSession sessions[8];
static MockHook hooks[16];
static int instance_count, set_count, action_count, session_count, installed_count;
static XrHandle next_handle;
static int active, hook_calls, hook_fail, unhook_calls, creates, fail_create, fail_path, fail_grip;
static int suggest_calls, reject_extra, last_count, polls, writes, sync_result;
static int gipa_calls, path_calls;
static int writer_result, sync_calls;
static XrHandle last_sync_session;
static const void* last_sync_info;
static int output[2], input_active[2], query_result[2];
static float input[2];
static int64_t time_ms;
static ActionCreateInfo created;
static XrHandle created_hands[2];
static SuggestedBinding received[4];

void* memcpy(void* out, const void* in, size_t n) {
    for (size_t i = 0; i < n; i++) ((char*)out)[i] = ((const char*)in)[i];
    return out;
}
void* memset(void* out, int value, size_t n) {
    for (size_t i = 0; i < n; i++) ((char*)out)[i] = (char)value;
    return out;
}
int strcmp(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}
int clock_gettime(int clock, struct timespec* out) {
    (void)clock;
    out->tv_sec = time_ms / 1000;
    out->tv_nsec = (time_ms % 1000) * 1000000;
    return 0;
}
int __android_log_print(int priority, const char* tag, const char* format, ...) {
    (void)priority; (void)tag; (void)format;
    return 0;
}
#ifndef BATTERY_LAYER_INTEGRATION
int vdhs_layer_active(void) { return active; }
#else
int battery_layer_install(void);
void battery_layer_reset(void);
XrResult battery_layer_create_instance(int result, XrHandle* out);
void* battery_layer_resolve(const char* name);
int battery_layer_observed(XrHandle grip, XrHandle eye);
#endif
int vdhs_managed_set_controller_battery(int left, int right) {
    writes++;
    if (writer_result == 1) { output[0] = left; output[1] = right; }
    return writer_result;
}

static MockInstance* find_instance(XrHandle handle) {
    for (int i = 0; i < instance_count; i++)
        if (instances[i].handle == handle && instances[i].alive) return &instances[i];
    return NULL;
}
static MockSet* find_set(XrHandle handle) {
    for (int i = 0; i < set_count; i++)
        if (sets[i].handle == handle && find_instance(sets[i].instance)) return &sets[i];
    return NULL;
}
static MockAction* find_action(XrHandle handle) {
    for (int i = 0; i < action_count; i++)
        if (actions[i].handle == handle && find_instance(actions[i].instance)) return &actions[i];
    return NULL;
}
static MockSession* find_session(XrHandle handle) {
    for (int i = 0; i < session_count; i++)
        if (sessions[i].handle == handle && find_instance(sessions[i].instance)) return &sessions[i];
    return NULL;
}
XrHandle mock_open_instance(void) {
    MockInstance* instance = &instances[instance_count++];
    instance->handle = ++next_handle;
    instance->alive = 1;
    for (int i = 0; i < 8; i++) instance->paths[i] = ++next_handle;
    return instance->handle;
}
static XrHandle mock_new_set(XrHandle instance) {
    MockSet* set = &sets[set_count++];
    set->handle = ++next_handle; set->instance = instance;
    return set->handle;
}
static XrHandle mock_new_session(XrHandle instance) {
    MockSession* session = &sessions[session_count++];
    session->handle = ++next_handle; session->instance = instance;
    return session->handle;
}
static XrResult mock_create(XrHandle set_handle, const ActionCreateInfo* info, XrHandle* out) {
    MockSet* set = find_set(set_handle);
    if (!set) return INVALID_HANDLE;
    if (set->attached) return -1; // Action sets are immutable after attachment.
    if (info->type != 29) return -1;
    int battery = !strcmp(info->actionName, "vdhs_battery");
    if (battery) {
        creates++;
        created = *info;
        if (info->countSubactionPaths != 2 || !info->subactionPaths) return -1;
        MockInstance* instance = find_instance(set->instance);
        for (int i = 0; i < 2; i++) {
            created_hands[i] = info->subactionPaths[i];
            if (created_hands[i] != instance->paths[i + 1]) return INVALID_HANDLE;
        }
    }
    if ((battery && fail_create) || (fail_grip && !strcmp(info->actionName, "grip_pose"))) {
        *out = 0;
        return -1;
    }
    MockAction* action = &actions[action_count++];
    action->handle = ++next_handle; action->instance = set->instance;
    action->set = set_handle; action->battery = battery;
    *out = action->handle;
    return 0;
}
static XrResult mock_path(XrHandle handle, const char* path, XrHandle* out) {
    path_calls++;
    MockInstance* instance = find_instance(handle);
    if (!instance) return INVALID_HANDLE;
    if (fail_path) return -19;
    const char* paths[] = {
        "/interaction_profiles/pico/neo3_controller",
        "/user/hand/left", "/user/hand/right",
        "/user/hand/left/input/battery/value", "/user/hand/right/input/battery/value",
        "/interaction_profiles/test/other_controller",
        "/user/hand/left/input/trigger/value", "/user/hand/right/input/trigger/value",
    };
    for (int i = 0; i < 8; i++)
        if (!strcmp(path, paths[i])) { *out = instance->paths[i]; return 0; }
    return -19;
}
static int contains(const XrHandle* handles, uint32_t count, XrHandle handle) {
    for (uint32_t i = 0; i < count; i++) if (handles[i] == handle) return 1;
    return 0;
}
static XrResult mock_get_float(XrHandle session_handle, const ActionStateGetInfo* info, ActionStateFloat* state) {
    MockSession* session = find_session(session_handle);
    MockAction* action = find_action(info->action);
    if (!session || !action || action->instance != session->instance) return INVALID_HANDLE;
    if (!contains(session->attached, session->attached_count, action->set)) return NOT_ATTACHED;
    if (!contains(session->synced, session->synced_count, action->set)) return NOT_SYNCED;
    if (info->type != 58 || state->type != 24) return -1;
    MockInstance* instance = find_instance(session->instance);
    int hand = info->subactionPath == instance->paths[1] ? 0 :
               info->subactionPath == instance->paths[2] ? 1 : -1;
    if (hand < 0) return INVALID_HANDLE;
    polls++;
    state->currentState = input[hand];
    state->isActive = (uint32_t)(input_active[hand] && (action->bound & (1 << hand)));
    return query_result[hand];
}
void* mock_loader_entry(const char* name);
static XrResult mock_gipa(XrHandle instance, const char* name, vdhs_xr_function* out) {
    gipa_calls++;
    if (!find_instance(instance)) return INVALID_HANDLE;
    if (!strcmp(name, "xrCreateAction")) *out = (vdhs_xr_function)mock_loader_entry(name);
    else if (!strcmp(name, "xrStringToPath")) *out = (vdhs_xr_function)mock_path;
    else if (!strcmp(name, "xrGetActionStateFloat")) *out = (vdhs_xr_function)mock_get_float;
    else return -7;
    return 0;
}
static XrResult mock_suggest(XrHandle handle, const SuggestedBindings* info) {
    MockInstance* instance = find_instance(handle);
    if (!instance) return INVALID_HANDLE;
    if (info->type != 51 || (info->interactionProfile != instance->paths[0] &&
                            info->interactionProfile != instance->paths[5])) return INVALID_HANDLE;
    suggest_calls++;
    last_count = (int)info->countSuggestedBindings;
    int has_battery = 0;
    for (int i = 0; i < last_count; i++) {
        SuggestedBinding binding = info->suggestedBindings[i];
        MockAction* action = find_action(binding.action);
        if (!action || action->instance != handle) return INVALID_HANDLE;
        if (!contains(instance->paths + 3, 2, binding.binding) &&
            !contains(instance->paths + 6, 2, binding.binding)) return INVALID_HANDLE;
        has_battery |= action->battery;
        if (i < 4) received[i] = binding;
    }
    if (reject_extra && has_battery) return -22;
    for (int i = 0; i < last_count; i++) {
        SuggestedBinding binding = info->suggestedBindings[i];
        int right = binding.binding == instance->paths[4] || binding.binding == instance->paths[7];
        find_action(binding.action)->bound |= 1 << right;
    }
    return 0;
}
static XrResult mock_attach(XrHandle handle, const AttachInfo* info) {
    MockSession* session = find_session(handle);
    if (!session) return INVALID_HANDLE;
    if (info->type != 60 || info->count > 2 || session->attached_count) return -1;
    for (uint32_t i = 0; i < info->count; i++) {
        MockSet* set = find_set(info->sets[i]);
        if (!set || set->instance != session->instance) return INVALID_HANDLE;
    }
    session->attached_count = info->count;
    for (uint32_t i = 0; i < info->count; i++) {
        session->attached[i] = info->sets[i];
        find_set(info->sets[i])->attached = 1;
    }
    return 0;
}
static XrResult mock_sync(XrHandle handle, const void* opaque) {
    sync_calls++;
    last_sync_session = handle;
    last_sync_info = opaque;
    MockSession* session = find_session(handle);
    if (!session) return INVALID_HANDLE;
    const SyncInfo* info = opaque;
    if (!info || info->type != 61 || info->count > 2) return -1;
    for (uint32_t i = 0; i < info->count; i++) {
        MockSet* set = find_set(info->sets[i].set);
        if (!set || set->instance != session->instance) return INVALID_HANDLE;
        if (!contains(session->attached, session->attached_count, set->handle)) return NOT_ATTACHED;
    }
    if (sync_result) return sync_result;
    session->synced_count = info->count;
    for (uint32_t i = 0; i < info->count; i++) session->synced[i] = info->sets[i].set;
    return 0;
}
void* vdhs_resolve_mapped_symbol(const char* library, const char* name) {
    if (strcmp(library, "/lib/arm64/libopenxr_loader.so")) return NULL;
    if (!strcmp(name, "xrSuggestInteractionProfileBindings")) return (void*)mock_suggest;
    if (!strcmp(name, "xrSyncActions")) return (void*)mock_sync;
    if (!strcmp(name, "xrCreateAction")) return (void*)mock_create;
    if (!strcmp(name, "xrGetInstanceProcAddr")) return (void*)mock_gipa;
#ifdef BATTERY_LAYER_INTEGRATION
    return battery_layer_resolve(name);
#else
    return NULL;
#endif
}
void* mock_loader_entry(const char* name) {
    void* address = vdhs_resolve_mapped_symbol("/lib/arm64/libopenxr_loader.so", name);
    for (int i = 0; i < installed_count; i++)
        if (hooks[i].installed && hooks[i].address == address) return hooks[i].replacement;
    return address;
}
void* shadowhook_hook_func_addr(void* address, void* replacement, void** original) {
    hook_calls++;
    if (hook_calls == hook_fail) return NULL;
    MockHook* hook = &hooks[installed_count++];
    hook->address = address; hook->replacement = replacement; hook->installed = 1;
    *original = address;
    return hook;
}
int shadowhook_unhook(void* stub) {
    ((MockHook*)stub)->installed = 0;
    unhook_calls++;
    return 0;
}
int shadowhook_get_errno(void) { return 1; }
const char* shadowhook_to_errmsg(int error) { (void)error; return "mock hook failure"; }

static void reset(void) {
    active = 1;
    hook_calls = hook_fail = unhook_calls = creates = fail_create = fail_path = fail_grip = 0;
    suggest_calls = reject_extra = last_count = polls = writes = sync_result = 0;
    gipa_calls = path_calls = 0;
    writer_result = 1; sync_calls = 0; last_sync_session = 0; last_sync_info = NULL;
    instance_count = set_count = action_count = session_count = installed_count = 0;
    next_handle = 100;
    memset(instances, 0, sizeof instances); memset(sets, 0, sizeof sets);
    memset(actions, 0, sizeof actions); memset(sessions, 0, sizeof sessions);
    memset(hooks, 0, sizeof hooks);
    output[0] = output[1] = -1;
    input[0] = 2.0f; input[1] = 5.0f;
    input_active[0] = input_active[1] = 1;
    query_result[0] = query_result[1] = 0;
    time_ms = 100;
    g_hooks_ready = 0;
    g_instance = g_action = g_profile = g_session = 0;
    g_bound = 0;
    g_percent[0] = g_percent[1] = -1;
    g_suggest = mock_suggest; g_sync = mock_sync;
    memset(&created, 0, sizeof created);
    memset(received, 0, sizeof received);
#ifdef BATTERY_LAYER_INTEGRATION
    battery_layer_reset();
#endif
}
static XrResult create_named(XrHandle set, const char* name, XrHandle* out) {
    ActionCreateInfo info = { .type = 29, .actionType = 4 };
    size_t n = 0;
    while (name[n]) { info.actionName[n] = name[n]; n++; }
    return ((CreateAction)mock_loader_entry("xrCreateAction"))(set, &info, out);
}
static int open_rig(Rig* rig, XrHandle instance) {
    rig->instance = instance;
    rig->set = mock_new_set(instance); rig->eye_set = mock_new_set(instance);
    rig->session = mock_new_session(instance);
    CHECK(create_named(rig->set, "left_trigger", &rig->original[0]) == 0);
    CHECK(create_named(rig->set, "right_trigger", &rig->original[1]) == 0);
    return 0;
}
static SuggestedBindings rig_bindings(const Rig* rig, SuggestedBinding* bindings, int other) {
    MockInstance* instance = find_instance(rig->instance);
    bindings[0] = (SuggestedBinding){ rig->original[0], instance->paths[6] };
    bindings[1] = (SuggestedBinding){ rig->original[1], instance->paths[7] };
    return (SuggestedBindings){ .type = 51, .interactionProfile = instance->paths[other ? 5 : 0],
        .countSuggestedBindings = 2, .suggestedBindings = bindings };
}
static XrResult suggest_rig(const Rig* rig) {
    SuggestedBinding bindings[2];
    SuggestedBindings info = rig_bindings(rig, bindings, 0);
    return ((Suggest)mock_loader_entry("xrSuggestInteractionProfileBindings"))(rig->instance, &info);
}
static XrResult attach_rig(const Rig* rig) {
    XrHandle attached[] = { rig->set, rig->eye_set };
    AttachInfo info = { .type = 60, .count = 2, .sets = attached };
    return mock_attach(rig->session, &info);
}
static XrResult sync_rig(const Rig* rig) {
    ActiveSet synced[] = { { rig->set, 0 }, { rig->eye_set, 0 } };
    SyncInfo info = { .type = 61, .count = 2, .sets = synced };
    return ((Sync)mock_loader_entry("xrSyncActions"))(rig->session, &info);
}
static XrResult read_action(XrHandle session, XrHandle action, XrHandle path) {
    ActionStateGetInfo info = { .type = 58, .action = action, .subactionPath = path };
    ActionStateFloat state = { .type = 24 };
    return mock_get_float(session, &info, &state);
}

// Repeat the whole lifecycle, not just the battery module's reset function.
static int recreate_rig(Rig* old, int integration) {
    XrHandle old_action = g_action, old_hand = g_hands[0];
    find_instance(old->instance)->alive = 0; // Destroys its sessions, sets and actions too.
    XrHandle instance;
#ifdef BATTERY_LAYER_INTEGRATION
    if (integration) CHECK(battery_layer_create_instance(0, &instance) == 0);
    else
#else
    (void)integration;
#endif
    { instance = mock_open_instance(); vdhs_battery_begin_instance(instance, mock_gipa); }
    CHECK(instance != old->instance && !g_action && !g_bound && g_percent[0] == -1 && g_percent[1] == -1);
    Rig next;
    CHECK(open_rig(&next, instance) == 0);
#ifdef BATTERY_LAYER_INTEGRATION
    if (integration) { XrHandle grip; CHECK(create_named(next.set, "grip_pose", &grip) == 0); }
    else
#endif
    vdhs_battery_on_grip_action(next.set);
    CHECK(g_action && g_action != old_action && g_hands[0] != old_hand);
    CHECK(find_action(g_action)->set == next.set);
    CHECK(suggest_rig(&next) == 0 && g_bound);
    CHECK(read_action(next.session, g_action, g_hands[0]) == NOT_ATTACHED);
    CHECK(attach_rig(&next) == 0);
    CHECK(read_action(next.session, g_action, g_hands[0]) == NOT_SYNCED);
    input[0] = 4.0f;
    int before = polls;
    CHECK(sync_rig(&next) == 0 && polls == before + 2 && output[0] == 80 && output[1] == 100);
    CHECK(read_action(next.session, old_action, g_hands[0]) == INVALID_HANDLE);
    CHECK(read_action(next.session, g_action, old_hand) == INVALID_HANDLE);
    CHECK(read_action(old->session, g_action, g_hands[0]) == INVALID_HANDLE);
    return 0;
}

__declspec(dllexport) int battery_case(int scenario) {
    reset();
    if (scenario == 4) hook_fail = 2;
    int install = vdhs_battery_install();
    Rig rig;
    CHECK(open_rig(&rig, mock_open_instance()) == 0);
    if (scenario == 4) {
        CHECK(install != 0 && !g_hooks_ready && unhook_calls == 1);
        vdhs_battery_begin_instance(rig.instance, mock_gipa);
        vdhs_battery_on_grip_action(rig.set);
        SuggestedBinding bindings[2];
        SuggestedBindings info = rig_bindings(&rig, bindings, 0);
        // A callback already in flight still uses its captured replacement after rollback.
        CHECK(((Suggest)hooks[0].replacement)(rig.instance, &info) == 0);
        CHECK(attach_rig(&rig) == 0 && sync_rig(&rig) == 0);
        CHECK(!creates && !writes && !polls && last_count == 2);
        return 0;
    }
    CHECK(install == 0 && hook_calls == 2);
    CHECK(mock_loader_entry("xrSuggestInteractionProfileBindings") == hooks[0].replacement);
    CHECK(mock_loader_entry("xrSyncActions") == hooks[1].replacement);
    if (scenario == 14) fail_path = 1;
    vdhs_battery_begin_instance(rig.instance, mock_gipa);
    if (scenario == 13) fail_create = 1;
    if (scenario != 24) vdhs_battery_on_grip_action(rig.set);
    if (scenario == 14) { CHECK(!creates); return 0; }
    if (scenario == 12) {
        vdhs_battery_on_grip_action(rig.set);
        CHECK(creates == 1 && !strcmp(created.actionName, "vdhs_battery") &&
              created.type == 29 && created.actionType == 2 && created.countSubactionPaths == 2 &&
              created_hands[0] == g_hands[0] && created_hands[1] == g_hands[1]);
        return 0;
    }
    SuggestedBinding original[2];
    SuggestedBindings info = rig_bindings(&rig, original, scenario == 1);
    if (scenario == 2) reject_extra = 1;
    if (scenario == 3) active = 0;
    CHECK(((Suggest)mock_loader_entry("xrSuggestInteractionProfileBindings"))(rig.instance, &info) == 0);
    CHECK(info.countSuggestedBindings == 2 && original[0].action == rig.original[0] &&
          original[1].binding == find_instance(rig.instance)->paths[7]);
    if (scenario == 1 || scenario == 2 || scenario == 3 || scenario == 13 || scenario == 24) {
        CHECK(attach_rig(&rig) == 0 && sync_rig(&rig) == 0);
        CHECK(!writes && !polls && last_count == 2);
        CHECK(suggest_calls == (scenario == 2 ? 2 : 1));
        CHECK(find_action(rig.original[0])->bound == 1 && find_action(rig.original[1])->bound == 2);
        return 0;
    }
    CHECK(last_count == 4 && received[0].action == rig.original[0] &&
          received[1].binding == original[1].binding && received[2].action == g_action &&
          received[2].binding == g_sources[0] && received[3].binding == g_sources[1]);
    if (scenario == 17) {
        CHECK(read_action(rig.session, g_action, g_hands[0]) == NOT_ATTACHED);
        CHECK(sync_rig(&rig) == NOT_ATTACHED && !polls);
        CHECK(attach_rig(&rig) == 0);
        CHECK(read_action(rig.session, g_action, g_hands[0]) == NOT_SYNCED);
        ActiveSet eye = { rig.eye_set, 0 };
        SyncInfo eye_only = { .type = 61, .count = 1, .sets = &eye };
        CHECK(mock_sync(rig.session, &eye_only) == 0);
        CHECK(read_action(rig.session, g_action, g_hands[0]) == NOT_SYNCED);
        time_ms += 1000;
    } else CHECK(attach_rig(&rig) == 0);
    if (scenario >= 21 && scenario <= 23) {
        ActiveSet enabled[] = { { rig.set, 0 }, { rig.eye_set, 0 } };
        SyncInfo sync = { .type = 61, .count = 2, .sets = enabled };
        if (scenario == 21) input_active[0] = input_active[1] = 0;
        else {
            writer_result = scenario == 22 ? 0 : -1;
            output[0] = 31; output[1] = 32;
        }
        for (int i = 0; i < 10000; ++i) {
            time_ms = 100 + i * 10;
            CHECK(((Sync)mock_loader_entry("xrSyncActions"))(rig.session, &sync) == 0);
            CHECK(last_sync_session == rig.session && last_sync_info == &sync);
            CHECK(sync.count == 2 && sync.sets == enabled && enabled[0].set == rig.set && enabled[1].set == rig.eye_set);
            CHECK(find_action(rig.original[0])->bound == 1 && find_action(rig.original[1])->bound == 2);
        }
        CHECK(sync_calls == 10000 && writes == 10000 && polls == 200);
        if (scenario == 21) CHECK(output[0] == -1 && output[1] == -1);
        else CHECK(output[0] == 31 && output[1] == 32);
        sync_result = 8;
        CHECK(((Sync)mock_loader_entry("xrSyncActions"))(rig.session, &sync) == 8);
        CHECK(last_sync_info == &sync && last_sync_session == rig.session);
        return 0;
    }
    if (scenario == 18) {
        XrHandle late = 0;
        CHECK(create_named(rig.set, "late_action", &late) != 0 && !late);
    }
    if (scenario == 5) { input_active[0] = 0; query_result[1] = -1; }
    if (scenario == 6) { input[0] = -0.1f; input[1] = 5.1f; }
    if (scenario == 19) {
        union { uint32_t bits; float value; } nan = { .bits = 0x7fc00000u };
        input[0] = 255.0f; input[1] = nan.value;
    }
    if (scenario == 20) input[0] = 0.0f;
    if (scenario == 9) sync_result = -16;
    CHECK(sync_rig(&rig) == sync_result);
    if (scenario == 5 || scenario == 6 || scenario == 9 || scenario == 19) {
        CHECK(output[0] == -1 && output[1] == -1 && polls == (scenario == 9 ? 0 : 2));
        return 0;
    }
    if (scenario == 20) { CHECK(output[0] == 0 && output[1] == 100 && polls == 2); return 0; }
    CHECK(output[0] == 40 && output[1] == 100 && polls == 2 && writes == (scenario == 17 ? 2 : 1));
    if (scenario == 7) {
        input[0] = 4.0f;
        time_ms = 1099; CHECK(sync_rig(&rig) == 0);
        CHECK(polls == 2 && output[0] == 40 && writes == 2);
        time_ms = 1100; CHECK(sync_rig(&rig) == 0);
        CHECK(polls == 4 && output[0] == 80);
    }
    if (scenario == 8) {
        rig.session = mock_new_session(rig.instance);
        CHECK(attach_rig(&rig) == 0);
        input[0] = 4.0f;
        CHECK(sync_rig(&rig) == 0 && polls == 4 && output[0] == 80);
    }
    if (scenario == 10) {
        output[0] = output[1] = 0;
        CHECK(sync_rig(&rig) == 0 && polls == 2 && writes == 2 && output[0] == 40 && output[1] == 100);
    }
    if (scenario == 11) return recreate_rig(&rig, 0);
    if (scenario == 15 || scenario == 16) {
        XrHandle old_action = g_action, old_hand = g_hands[0], ignored;
        vdhs_xr_function fn = NULL;
        if (scenario == 15) find_instance(rig.instance)->alive = 0;
        Rig other;
        CHECK(open_rig(&other, mock_open_instance()) == 0);
        CHECK(mock_attach(other.session, &(AttachInfo){ .type = 60, .count = 1, .sets = &rig.set }) == INVALID_HANDLE);
        CHECK(attach_rig(&other) == 0);
        CHECK(mock_sync(other.session, &(SyncInfo){ .type = 61, .count = 1,
              .sets = (ActiveSet[]){ { rig.set, 0 } } }) == INVALID_HANDLE);
        CHECK(read_action(other.session, old_action, old_hand) == INVALID_HANDLE);
        if (scenario == 16)
            CHECK(read_action(rig.session, old_action, find_instance(other.instance)->paths[1]) == INVALID_HANDLE);
        SuggestedBinding foreign[] = { { old_action, find_instance(other.instance)->paths[3] } };
        SuggestedBindings foreign_info = { .type = 51, .interactionProfile = find_instance(other.instance)->paths[0],
            .countSuggestedBindings = 1, .suggestedBindings = foreign };
        CHECK(mock_suggest(other.instance, &foreign_info) == INVALID_HANDLE);
        CHECK(mock_suggest(other.instance, &info) == INVALID_HANDLE);
        if (scenario == 15) {
            CHECK(mock_gipa(rig.instance, "xrCreateAction", &fn) == INVALID_HANDLE);
            CHECK(mock_path(rig.instance, "/user/hand/left", &ignored) == INVALID_HANDLE);
            CHECK(create_named(rig.set, "expired_action", &ignored) == INVALID_HANDLE);
            CHECK(mock_suggest(rig.instance, &info) == INVALID_HANDLE);
            CHECK(attach_rig(&rig) == INVALID_HANDLE && sync_rig(&rig) == INVALID_HANDLE);
            CHECK(read_action(rig.session, old_action, old_hand) == INVALID_HANDLE);
        }
    }
    return 0;
}

#ifdef BATTERY_LAYER_INTEGRATION
__declspec(dllexport) int battery_integration_case(int scenario) {
    reset();
    if (scenario == 3) hook_fail = 12; // Ten core hooks, then suggest and sync.
    CHECK(battery_layer_install() == 0 && vdhs_layer_active());
    CHECK(hook_calls == 12);
    XrHandle instance = 0;
    CHECK(battery_layer_create_instance(scenario == 1 ? -1 : 0, &instance) == (scenario == 1 ? -1 : 0));
    if (scenario == 1) {
        CHECK(!instance && !g_instance && !g_action && !gipa_calls && !path_calls);
        return 0;
    }
    CHECK(instance && (scenario == 3 ? !g_instance : g_instance == instance));
    CHECK(gipa_calls == (scenario == 3 ? 0 : 3) && path_calls == (scenario == 3 ? 0 : 5));
    Rig rig;
    CHECK(open_rig(&rig, instance) == 0 && !creates);
    XrHandle grip = 0, eye = 0;
    fail_grip = scenario == 2;
    CHECK(create_named(rig.set, "grip_pose", &grip) == (fail_grip ? -1 : 0));
    if (scenario == 2) { CHECK(!grip && !creates && !g_action); return 0; }
    CHECK(create_named(rig.eye_set, "eye_gaze_pose", &eye) == 0);
    CHECK(battery_layer_observed(grip, eye));
    CHECK(grip && grip != g_action && find_action(grip)->set == rig.set);
    if (scenario == 3) {
        CHECK(!g_hooks_ready && !creates && !g_action && unhook_calls == 1);
        CHECK(suggest_rig(&rig) == 0 && last_count == 2);
        CHECK(attach_rig(&rig) == 0 && sync_rig(&rig) == 0 && !polls && !writes);
        return 0;
    }
    CHECK(creates == 1 && find_action(g_action)->set == rig.set);
    CHECK(suggest_rig(&rig) == 0 && last_count == 4);
    CHECK(find_action(rig.original[0])->bound == 1 && find_action(rig.original[1])->bound == 2);
    if (scenario >= 5 && scenario <= 7) {
        input_active[0] = input_active[1] = 0;
        writer_result = scenario == 5 ? 1 : scenario == 6 ? 0 : -1;
        output[0] = 31; output[1] = 32;
        CHECK(attach_rig(&rig) == 0);
        for (int i = 0; i < 1000; ++i) {
            time_ms = 100 + i * 10;
            CHECK(sync_rig(&rig) == 0 && vdhs_layer_active());
            CHECK(find_session(rig.session)->synced_count == 2);
            CHECK(find_action(rig.original[0])->bound == 1 && find_action(rig.original[1])->bound == 2);
        }
        CHECK(sync_calls == 1000 && writes == 1000 && polls == 20);
        if (scenario == 5) CHECK(output[0] == -1 && output[1] == -1);
        else CHECK(output[0] == 31 && output[1] == 32);
        return 0;
    }
    CHECK(attach_rig(&rig) == 0 && sync_rig(&rig) == 0);
    CHECK(polls == 2 && writes == 1 && output[0] == 40 && output[1] == 100);
    if (scenario == 4) return recreate_rig(&rig, 1);
    return 0;
}
#endif
