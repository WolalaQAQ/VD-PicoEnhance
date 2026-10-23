extern "C" {
#include "input_probe.h"
}
#include "worker.h"
#include <intrin.h>
#include <stddef.h>

struct Writer { char *next; };

static void text(Writer &w, const char *value) {
    while (*value) *w.next++ = *value++;
}
static void number(Writer &w, uint64_t value) {
    char digits[20];
    uint32_t count = 0;
    do { digits[count++] = static_cast<char>('0' + value % 10); value /= 10; } while (value);
    while (count) *w.next++ = digits[--count];
}
static void boolean(Writer &w, bool value) { text(w, value ? "true" : "false"); }
static uint32_t float_bits(float value) {
    uint32_t bits;
    __builtin_memcpy(&bits, &value, sizeof(bits));
    return bits;
}
static void vector_bits(Writer &w, const vr::HmdVector3_t &value) {
    text(w, "[");
    for (uint32_t axis = 0; axis < 3; ++axis) {
        if (axis) text(w, ",");
        number(w, float_bits(value.v[axis]));
    }
    text(w, "]");
}
static void phase_fields(Writer &w, const MvWorkerContext *c, uint64_t now) {
    static const char *const labels[] = {"unknown", "index", "hands", "stop", "auto", "restore"};
    text(w, ",\"timestamp_ms\":"); number(w, now);
    text(w, ",\"phase_id\":"); number(w, c->phase_id);
    text(w, ",\"phase_label\":\""); text(w, labels[c->phase]); text(w, "\"");
}
static void emit(MvWorkerContext *c, Writer &w) {
    *w.next = 0;
    c->bindings.log_sink(c->bindings.user, c->line);
}

static void sample_line(MvWorkerContext *c, uint32_t index, uint64_t now) {
    const MvSample &s = c->samples[index];
    const vr::TrackedDevicePose_t &pose = c->sdk.raw[index];
    Writer w{c->line};
    text(w, "MVRAW {\"type\":\"sample\""); phase_fields(w, c, now);
    text(w, ",\"index\":"); number(w, index);
    text(w, ",\"identity_generation\":"); number(w, c->sdk.devices[index].generation);
    text(w, ",\"role\":"); number(w, s.role);
    text(w, ",\"role_source\":\"hint\",\"known\":"); boolean(w, s.known);
    text(w, ",\"touched\":"); boolean(w, s.touched);
    text(w, ",\"updates\":"); number(w, s.updates);
    text(w, ",\"updates_in_phase\":"); number(w, s.updates_in_phase);
    text(w, ",\"observed_this_phase\":"); boolean(w, s.observed_this_phase);
    text(w, ",\"last_update_ms\":"); number(w, s.last_update_ms);
    text(w, ",\"pose_known\":"); boolean(w, s.pose_known);
    text(w, ",\"moving\":"); boolean(w, s.moving);
    text(w, ",\"stationary\":"); boolean(w, s.stationary);
    text(w, ",\"index_candidate\":"); boolean(w, s.index_candidate);
    text(w, ",\"pose\":{\"connected\":"); boolean(w, pose.bDeviceIsConnected);
    text(w, ",\"valid\":"); boolean(w, pose.bPoseIsValid);
    text(w, ",\"tracking_result\":"); number(w, static_cast<uint32_t>(pose.eTrackingResult));
    text(w, ",\"velocity_f32_bits\":"); vector_bits(w, pose.vVelocity);
    text(w, ",\"angular_velocity_f32_bits\":"); vector_bits(w, pose.vAngularVelocity);
    text(w, "}}");
    emit(c, w);
}

static bool has_mapping(MvProbe *probe, uint64_t container) {
    while (_InterlockedCompareExchange(&probe->lock, 1, 0)) _mm_pause();
    bool found = false;
    for (const auto &entry : probe->components)
        if (entry.component && entry.container == container) { found = true; break; }
    _InterlockedExchange(&probe->lock, 0);
    return found;
}

void mv_worker_init(MvWorkerContext *c, const MvWorkerBindings *bindings) {
    auto *bytes = reinterpret_cast<unsigned char *>(c);
    for (size_t i = 0; i < sizeof(*c); ++i) bytes[i] = 0;
    c->bindings = *bindings;
    mv_sdk_init(&c->sdk, bindings->probe, bindings->properties, bindings->host);
}

void mv_worker_set_phase(MvWorkerContext *c, uint64_t sequence, MvWorkerPhase phase) {
    if (c->exit_requested || sequence <= c->phase_id) return;
    mv_mark_phase(c->bindings.probe);
    c->phase_id = sequence;
    c->phase = phase;
    c->exit_requested = phase == MV_WORKER_STOP;
}

uint32_t mv_worker_tick(MvWorkerContext *c) {
    if (c->exit_requested) return 0;
    const uint64_t now = c->bindings.clock_ms(c->bindings.user);
    if (c->sampled && now < c->next_sample_ms) return 0;
    c->sampled = true;
    c->next_sample_ms = now + MV_WORKER_INTERVAL_MS;
    const uint32_t identities = mv_sdk_sample(&c->sdk, c->samples);
    uint32_t selected[2] = {MV_SDK_DEVICE_COUNT, MV_SDK_DEVICE_COUNT};
    uint32_t policy_selected[2] = {MV_SDK_DEVICE_COUNT, MV_SDK_DEVICE_COUNT};
    uint32_t counts[2] = {};
    c->connected_index_devices = 0;
    uint32_t excluded = 0, missing = 0, attempts = 0, recovered = 0;
    for (uint32_t index = 0; index < MV_SDK_DEVICE_COUNT; ++index) {
        const auto &device = c->sdk.devices[index];
        if (!device.ready) {
            if (c->sdk.raw[index].bDeviceIsConnected) ++excluded;
            /* A previously identified, still-connected side with temporarily
             * unreadable identity is unknown, not a released/absent side. */
            if (device.generation && c->sdk.raw[index].bDeviceIsConnected)
                ++counts[device.role - 1];
            continue;
        }
        uint32_t &chosen = selected[device.role - 1];
        if (c->sdk.raw[index].bDeviceIsConnected) {
            ++c->connected_index_devices;
            ++counts[device.role - 1];
            policy_selected[device.role - 1] = index;
        }
        if (chosen == MV_SDK_DEVICE_COUNT) chosen = index;
        if (has_mapping(c->bindings.probe, device.container)) continue;
        ++missing;
        if (c->bindings.recover && now >= c->recover_after_ms[index]) {
            c->recover_after_ms[index] = now + MV_WORKER_RECOVER_MS;
            ++attempts;
            uint64_t component = 0;
            if (c->bindings.recover(c->bindings.user, device.container, &component) == 1 && component) {
                mv_created(c->bindings.probe, device.container, MV_TRACKPAD_TOUCH, component, 0);
                ++recovered;
            }
        }
    }
    uint32_t emitted = 0;
    MvSwitchInput inputs[2] = {};
    for (uint32_t side = 0; side < 2; ++side) {
        inputs[side].present = counts[side] != 0;
        if (counts[side] != 1) continue;
        const uint32_t index = policy_selected[side];
        if (index == MV_SDK_DEVICE_COUNT) continue;
        const auto &device = c->sdk.devices[index];
        inputs[side] = {device.container, device.generation, index, 1, c->samples[index]};
    }
    mv_switch_tick(&c->switching, inputs, now);
    for (uint32_t index : selected) {
        if (index == MV_SDK_DEVICE_COUNT) continue;
        sample_line(c, index, now);
        ++emitted;
    }
    Writer w{c->line};
    text(w, "MVRAW {\"type\":\"status\""); phase_fields(w, c, now);
    text(w, ",\"real_index_devices\":"); number(w, identities);
    text(w, ",\"connected_index_devices\":"); number(w, c->connected_index_devices);
    text(w, ",\"emitted_devices\":"); number(w, emitted);
    text(w, ",\"excluded_connected_devices\":"); number(w, excluded);
    text(w, ",\"missing_components\":"); number(w, missing);
    text(w, ",\"recovery_attempts\":"); number(w, attempts);
    text(w, ",\"recovery_found\":"); number(w, recovered);
    text(w, "}");
    emit(c, w);
    return emitted + 1;
}
