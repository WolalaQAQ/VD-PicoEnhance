// Reuse only the owned SDK fake classes; do not execute their main test.
#define main owned_controller_test_main
#include "controller_fixture.cpp"
#undef main
#include <windows.h>
#include <atomic>

namespace {
std::atomic<unsigned> logs{0};
std::atomic<unsigned> searches{0}, enabled_sessions{0};
class FakeLog final : public vr::IVRDriverLog {
    void Log(const char *line) override {
        ++logs;
        if (strstr(line, "\"state\":\"searching\"")) ++searches;
        if (strstr(line, "\"state\":\"enabled\"")) ++enabled_sessions;
    }
};
// Explicit prefix ABI calls prevent compiler devirtualization from skipping the
// actual image-owned table pointers installed by the provider's capture layer.
vr::EVRInputError make(void *, uint64_t, const char *, uint64_t *out) { *out = 100; return vr::VRInputError_None; }
vr::EVRInputError update(void *, uint64_t, bool, double) { return vr::VRInputError_None; }
void *const table[] = {reinterpret_cast<void *>(&make), reinterpret_cast<void *>(&update)};
struct Input { void *const *vtable = table; } input;
class Context final : public vr::IVRDriverContext {
public:
    void *GetGenericInterface(const char *name, vr::EVRInitError *error) override {
        *error = vr::VRInitError_None;
        if (!strcmp(name, vr::IVRProperties_Version)) return &properties;
        if (!strcmp(name, vr::IVRServerDriverHost_Version)) return &host;
        if (!strcmp(name, vr::IVRDriverLog_Version)) return &logger;
        // Exercise the current runtime's input version with the complete driver;
        // capture/check.py separately covers 003/004 and mixed-version tables.
        if (!strcmp(name, "IVRDriverInput_005")) return &input;
        *error = vr::VRInitError_Init_InterfaceNotFound; return nullptr;
    }
    vr::DriverHandle_t GetDriverHandle() override { return 1; }
    FakeLog logger;
} context;
}
int main(int argc, char **argv) {
    if (argc != 2 && argc != 3) return 1;
    const auto dll = LoadLibraryA(argv[1]);
    if (!dll) return 2;
    using Factory = void *(*)(const char *, int *);
    const auto factory = reinterpret_cast<Factory>(GetProcAddress(dll, "HmdDriverFactory"));
    int error = 0;
    auto *provider = static_cast<vr::IServerTrackedDeviceProvider *>(factory(vr::IServerTrackedDeviceProvider_Version, &error));
    check(provider && !factory("wrong-interface", &error), "factory exposes only server provider");
    check(provider->Init(&context) == vr::VRInitError_None, "actual DLL initializes with owned SDK context");
    check(*reinterpret_cast<void *const *volatile *>(&input) != nullptr, "mock input object valid");
    auto *slots = *reinterpret_cast<void *** >(&input);
    const auto slot = [slots](unsigned index) { return *reinterpret_cast<void *volatile *>(&slots[index]); };
    check(slot(0) != reinterpret_cast<void *>(&make) && slot(1) != reinterpret_cast<void *>(&update), "actual capture patched shared mock table");
    uint64_t component = 0;
    using Create = vr::EVRInputError (*)(void *, uint64_t, const char *, uint64_t *);
    using Update = vr::EVRInputError (*)(void *, uint64_t, bool, double);
    check(reinterpret_cast<Create>(slot(0))(&input, 99, MV_TRACKPAD_TOUCH, &component) == vr::VRInputError_None && component == 100, "actual create wrapper forwards");
    check(reinterpret_cast<Update>(slot(1))(&input, component, true, 0.0) == vr::VRInputError_None, "actual update wrapper forwards");
    const bool reconnect = argc == 3 && !strcmp(argv[2], "reconnect");
    const bool stop_search = argc == 3 && !strcmp(argv[2], "stop-discovery");
    const bool protocol_failure = argc == 3 && (!strcmp(argv[2], "helper-error") || !strcmp(argv[2], "bad-ack"));
    const auto stop = [] {
        const auto path = std::string(getenv("LOCALAPPDATA")) + "/PicoMixedVR/phase.txt";
        auto *file = fopen(path.c_str(), "wb");
        check(file != nullptr, "owned Stop marker can be opened");
        check(fwrite("2 restore\n", 1, 10, file) == 10 && !fclose(file), "owned Stop marker saved");
    };
    if (stop_search) {
        const auto until = GetTickCount64() + 2000;
        while (!searches.load() && GetTickCount64() < until) Sleep(5);
        check(searches.load() == 1, "actual DLL started one asynchronous discovery");
        stop();
    }
    for (unsigned i = 0; i < (reconnect || protocol_failure ? 300u : 100u); ++i) { provider->RunFrame(); Sleep(20); }
    if (protocol_failure) {
        check(searches.load() == 1 && enabled_sessions.load() == 1,
              "explicit backend/protocol error does not automatically rediscover");
        stop(); Sleep(200);
    }
    if (reconnect) {
        check(enabled_sessions.load() >= 2, "actual DLL rediscovered and opened a fresh session");
        stop(); Sleep(200);
    }
    const auto saved_searches = searches.load();
    provider->EnterStandby(); Sleep(200);
    provider->LeaveStandby(); Sleep(stop_search || reconnect || protocol_failure ? 1200 : 100);
    if (stop_search || reconnect || protocol_failure)
        check(searches.load() == saved_searches, "saved Stop prevents discovery and standby rearm");
    if (stop_search) check(enabled_sessions.load() == 0, "Stop during discovery never starts TCP control");
    provider->Cleanup();
    check(property_writes == 0 && device_adds == 0, "idle provider creates no devices or priority/battery writes");
    check(logs.load() > 0, "actual provider emitted lifecycle diagnostics");
    check(reinterpret_cast<Update>(slot(1))(&input, component, false, 0.0) == vr::VRInputError_None, "after Cleanup cached wrapper remains passthrough");
    FreeLibrary(dll);
    check(GetModuleHandleA("driver_picomixedvr.dll") != nullptr, "callback DLL remains pinned until owned process exits");
    printf("{\"passed\":%u,\"actual_provider_loaded\":true,\"official_runtime_loaded\":false}\n", checks);
    return 0;
}
