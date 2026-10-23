#include "controller.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <climits>

namespace {
uint64_t now = 1000;
unsigned checks, property_writes, device_adds;
bool answer = true, fail_identity = false;
int opens, closes, mode;
std::string incoming;
std::vector<std::string> commands;
MvProbe probe;
MvStandalone controller;
struct Device { const char *driver = ""; const char *kind = ""; const char *serial = ""; int role = 0; int priority = INT_MIN; } devices[4];
vr::TrackedDevicePose_t poses[64];
void check(bool ok, const char *label) {
    ++checks;
    if (!ok) { fprintf(stderr, "FAIL %u: %s\n", checks, label); std::exit(1); }
}
class Properties final : public vr::IVRProperties {
public:
    vr::ETrackedPropertyError ReadPropertyBatch(uint64_t container, vr::PropertyRead_t *items, uint32_t n) override {
        if (container < 1 || container > 4 || (fail_identity && container == 3 && n != 1)) return vr::TrackedProp_InvalidDevice;
        auto &d = devices[container - 1];
        for (uint32_t i = 0; i < n; ++i) {
            auto &r = items[i]; const void *data = nullptr; uint32_t bytes = 0;
            int cls = vr::TrackedDeviceClass_Controller;
            r.unTag = vr::k_unStringPropertyTag;
            switch (r.prop) {
            case vr::Prop_DeviceClass_Int32: data = &cls; bytes = sizeof(cls); r.unTag = vr::k_unInt32PropertyTag; break;
            case vr::Prop_ControllerRoleHint_Int32: data = &d.role; bytes = sizeof(d.role); r.unTag = vr::k_unInt32PropertyTag; break;
            case vr::Prop_ActualTrackingSystemName_String: data = d.driver; bytes = static_cast<uint32_t>(strlen(d.driver) + 1); break;
            case vr::Prop_ControllerType_String: data = d.kind; bytes = static_cast<uint32_t>(strlen(d.kind) + 1); break;
            case vr::Prop_SerialNumber_String: data = d.serial; bytes = static_cast<uint32_t>(strlen(d.serial) + 1); break;
            case vr::Prop_ControllerHandSelectionPriority_Int32: data = &d.priority; bytes = sizeof(d.priority); r.unTag = vr::k_unInt32PropertyTag; break;
            default: return vr::TrackedProp_UnknownProperty;
            }
            r.unRequiredBufferSize = bytes;
            r.eError = bytes <= r.unBufferSize ? vr::TrackedProp_Success : vr::TrackedProp_BufferTooSmall;
            if (r.eError == vr::TrackedProp_Success) memcpy(r.pvBuffer, data, bytes);
        }
        return vr::TrackedProp_Success;
    }
    vr::ETrackedPropertyError WritePropertyBatch(uint64_t container, vr::PropertyWrite_t *items, uint32_t n) override {
        check(container == 3 || container == 4, "only HAND priority may be written");
        for (uint32_t i = 0; i < n; ++i) {
            auto &r = items[i];
            check(r.prop == vr::Prop_ControllerHandSelectionPriority_Int32 && r.writeType == vr::PropertyWrite_Set, "no input/battery property writes");
            devices[container - 1].priority = *static_cast<int *>(r.pvBuffer);
            r.eError = vr::TrackedProp_Success; ++property_writes;
        }
        return vr::TrackedProp_Success;
    }
    const char *GetPropErrorNameFromEnum(vr::ETrackedPropertyError) override { return "owned"; }
    uint64_t TrackedDeviceToPropertyContainer(uint32_t index) override { return index < 4 && devices[index].role ? index + 1 : 0; }
} properties;
class Host final : public vr::IVRServerDriverHost {
public:
    bool TrackedDeviceAdded(const char *, vr::ETrackedDeviceClass, vr::ITrackedDeviceServerDriver *) override { ++device_adds; return false; }
    void TrackedDevicePoseUpdated(uint32_t, const vr::DriverPose_t &, uint32_t) override {}
    void VsyncEvent(double) override {}
    void VendorSpecificEvent(uint32_t, vr::EVREventType, const vr::VREvent_Data_t &, double) override {}
    bool IsExiting() override { return false; }
    bool PollNextEvent(vr::VREvent_t *, uint32_t) override { return false; }
    void GetRawTrackedDevicePoses(float, vr::TrackedDevicePose_t *out, uint32_t n) override { memcpy(out, poses, n * sizeof(*out)); }
    void RequestRestart(const char *, const char *, const char *, const char *) override {}
    uint32_t GetFrameTimings(vr::Compositor_FrameTiming *, uint32_t) override { return 0; }
    void SetDisplayEyeToHead(uint32_t, const vr::HmdMatrix34_t &, const vr::HmdMatrix34_t &) override {}
    void SetDisplayProjectionRaw(uint32_t, const vr::HmdRect2_t &, const vr::HmdRect2_t &) override {}
    void SetRecommendedRenderTargetSize(uint32_t, uint32_t, uint32_t) override {}
} host;
uint64_t clock_ms(void *) { return now; }
void log(void *, const char *) {}
int open(void *, uint16_t port) { check(port == 29744, "configured port forwarded to transport"); ++opens; return 1; }
int poll(void *) { return 1; }
void close(void *) { ++closes; incoming.clear(); }
int random(void *, unsigned char *out, uint32_t n) { memset(out, 0x62, n); return 1; }
void event(void *, const MvHeadsetEvent *e) { mv_standalone_event(&controller, e); }
int send(void *, const char *bytes, uint32_t n) {
    std::string line(bytes, n);
    unsigned long long sequence;
    char command[16];
    check(sscanf(line.c_str(), "VDM1 %*64s %*32s %llu %15s", &sequence, command) == 2, "existing VDM1 protocol");
    commands.emplace_back(command);
    if (answer) {
        if (!strcmp(command, "index")) mode = 1;
        if (!strcmp(command, "hands")) mode = 2;
        if (!strcmp(command, "restore")) mode = 0;
        char reply[128];
        snprintf(reply, sizeof(reply), "VDR1 %llu %d %d unobserved ok\n", sequence, mode, mode == 1 ? 0 : 1);
        incoming += reply;
    }
    return static_cast<int>(n);
}
int receive(void *, char *bytes, uint32_t capacity) {
    if (incoming.empty()) return MV_HEADSET_IO_WAIT;
    const auto n = incoming.size() < capacity ? incoming.size() : capacity;
    memcpy(bytes, incoming.data(), n); incoming.erase(0, n);
    return static_cast<int>(n);
}
MvHeadsetBindings io{nullptr, open, poll, send, receive, close, random, clock_ms, event};
void step(unsigned milliseconds = 100) {
    now += milliseconds;
    mv_standalone_tick(&controller, now);
}
void start() {
    check(mv_standalone_start(&controller, &io, 29744, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"), "explicit start succeeds");
    step(); check(controller.ready, "execution STATUS enables readiness");
}
void reset() {
    now = 1000; mode = opens = closes = 0; property_writes = device_adds = 0;
    answer = true; fail_identity = false; incoming.clear(); commands.clear();
    memset(poses, 0, sizeof(poses));
    for (auto &d : devices) d = {};
    mv_reset(&probe);
    const MvWorkerBindings bindings{&probe, &properties, &host, log, clock_ms, nullptr, nullptr};
    mv_standalone_init(&controller, &bindings, nullptr);
}
void device(unsigned index, const char *driver, const char *kind, const char *serial, unsigned role) {
    devices[index] = {driver, kind, serial, static_cast<int>(role), INT_MIN};
    poses[index].bDeviceIsConnected = poses[index].bPoseIsValid = true;
    poses[index].eTrackingResult = vr::TrackingResult_Running_OK;
    mv_created(&probe, index + 1, MV_TRACKPAD_TOUCH, index + 100, 0);
}
void touch(unsigned index, bool value, float speed) {
    mv_updated(&probe, index + 100, value, 0, now);
    poses[index].vVelocity.v[0] = speed;
}
unsigned count(const char *value) { unsigned n = 0; for (const auto &c : commands) if (c == value) ++n; return n; }
void finish() {
    mv_standalone_stop(&controller, now); step(20);
    check(controller.client.state == MV_HEADSET_CLOSED, "explicit stop closes after restore receipt");
}
}
int main() {
    reset(); start();
    for (unsigned i = 0; i < 70; ++i) step();
    check(count("status") > 10 && !count("index") && !count("hands"), "no-controller idle stays STATUS-only beyond lease");
    check(property_writes == 0 && device_adds == 0 && controller.automatic, "idle never touches HAND or adds devices");
    finish();

    reset(); device(0, "lighthouse", "knuckles", "OWNED-L", 1);
    device(1, "lighthouse", "knuckles", "OWNED-R", 2);
    device(2, "VirtualDesktop", "vd_hand_controller", "HANDL", 1);
    device(3, "VirtualDesktop", "vd_hand_controller", "HANDR", 2);
    start();
    touch(0, true, 0); touch(1, false, .10f); step();
    check(!count("index") && !property_writes, "left touch plus right movement is not pickup");
    touch(0, true, .10f); step();
    check(mode == 1 && count("index") == 1, "same-device touch AND movement applies Index");
    check(devices[2].priority == 100 && devices[3].priority == 100, "existing HAND preference applied");
    touch(0, false, 0); touch(1, false, 0); step();
    for (unsigned i = 0; i < 9; ++i) step();
    check(mode == 1, "release does not switch before 1000ms");
    step(); check(mode == 2 && count("hands") == 1, "both released stationary 1000ms applies Hands");
    touch(1, true, .10f); step(); check(mode == 1, "either real controller can pick up");
    poses[0].bDeviceIsConnected = poses[1].bDeviceIsConnected = false; step();
    check(mode == 0 && count("restore") == 1 && !controller.requested, "source loss uses distinct Restore");
    check(devices[2].priority == INT_MIN && devices[3].priority == INT_MIN, "source loss restores original HAND baselines");
    const auto index_count = count("index");
    for (unsigned i = 0; i < 40; ++i) step();
    check(mode == 0 && count("index") == index_count, "old Index heartbeat cancelled after source loss");
    poses[0].bDeviceIsConnected = true; step(); touch(0, false, 0);
    step(); for (unsigned i = 0; i < 10; ++i) step();
    check(mode == 2, "remaining single controller can release after source-loss Restore");
    fail_identity = true; mv_standalone_stop(&controller, now); step(20);
    check(mv_standalone_restore_pending(&controller), "temporary identity failure preserves original baseline");
    check(!mv_standalone_start(&controller, &io, 29744, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"), "pending restore blocks recapture");
    fail_identity = false; step(1000);
    check(!mv_standalone_restore_pending(&controller) && devices[2].priority == INT_MIN, "retry restores original not residual100");

    reset(); device(0, "lighthouse", "knuckles", "OWNED-L", 1);
    device(2, "VirtualDesktop", "vd_hand_controller", "HANDL", 1); start();
    touch(0, true, .10f); step();
    fail_identity = true; poses[0].bDeviceIsConnected = false; step(); step();
    check(!controller.automatic && mv_standalone_restore_pending(&controller), "failed source-loss recovery locks auto even while all sources absent");
    fail_identity = false; step(1000);
    poses[0].bDeviceIsConnected = true; touch(0, true, .10f); step();
    check(!controller.automatic && devices[2].priority == INT_MIN && mode == 0, "successful recovery does not silently rearm on reconnect");

    reset(); device(0, "lighthouse", "knuckles", "OWNED-L", 1); start();
    for (unsigned i = 0; i < 30; ++i) step();
    check(!count("hands"), "unknown touch is never synthetic release");
    touch(0, true, .10f); step(); check(mode == 1, "single pickup works");
    answer = false;
    for (unsigned i = 0; i < 30; ++i) step();
    check(!controller.automatic && controller.client.state == MV_HEADSET_FAILED, "missing ACK locks automatic switching");
    const auto open_count = opens;
    answer = true; for (unsigned i = 0; i < 40; ++i) step();
    check(opens == open_count, "input changes never reconnect failed session");
    start(); check(opens == open_count + 1, "fresh explicit auto may reconnect"); finish();

    reset(); device(0, "VirtualDesktop", "knuckles", "SIMULATED", 1); start();
    touch(0, true, .10f); step();
    check(!count("index") && !property_writes, "simulated knuckles not real Lighthouse"); finish();
    printf("{\"passed\":%u,\"scope\":\"owned SDK + actual controller/client/core\"}\n", checks);
    return 0;
}
