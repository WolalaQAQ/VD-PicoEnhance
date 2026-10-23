#include "worker.h"
#include "capture.h"
#include "hand_preference.h"
#include "frame_transport.h"
#include "writer_hook.h"
#include "native_frame.h"
#include "runtime.h"
#include <intrin.h>

#ifndef VD_ENTRY_RVA
#error VD_ENTRY_RVA is required
#endif
extern "C" void vd_native_run_frame(void *self);

namespace {
MvWorkerContext worker;
MvHandPreference hand_preference;
MvNativeApi api;
MvFrameTransport frame;
MvWriterHook writer;
VdNativeProvider *provider;
void *driver_reference;
bool automatic, frame_initialized, writer_available, headset_ready, phase_started;
uint32_t requested_mode;
vr::IVRProperties *driver_properties;
vr::IVRDriverLog *driver_log;
vr::IVRDriverInput *driver_input;
uint64_t next_diagnostic_ms;
uint64_t next_preference_ms;
uint64_t session_ms;
uint32_t process_id;
wchar_t phase_path[1024];
uintptr_t base();

bool config_paths(const MvNativeApi &bindings) {
    using GetProc = void *(*)(void *, const char *);
    const auto get_proc = *reinterpret_cast<GetProc *>(base() + 0x7D0F8);
    const auto kernel = bindings.module(L"kernel32.dll");
    using Environment = uint32_t (*)(const wchar_t *, wchar_t *, uint32_t);
    const auto environment = kernel ? reinterpret_cast<Environment>(get_proc(kernel, "GetEnvironmentVariableW")) : nullptr;
    if (!environment) return false;
    const uint32_t length = environment(L"LOCALAPPDATA", phase_path, 900);
    if (!length || length >= 900) return false;
    const wchar_t suffix[] = L"\\VD-PicoEnhance\\";
    uint32_t end = length;
    for (uint32_t i = 0; suffix[i]; ++i) phase_path[end++] = suffix[i];
    const wchar_t phase_name[] = L"phase.txt";
    for (uint32_t i = 0; i < sizeof(phase_name) / sizeof(wchar_t); ++i) phase_path[end + i] = phase_name[i];
    return true;
}

uintptr_t base() { return reinterpret_cast<uintptr_t>(&vd_native_run_frame) - VD_ENTRY_RVA; }
void sink(void *, const char *line);
uint64_t clock_ms(void *) { return api.clock_ms(); }

char *text(char *out, const char *value) {
    while (*value) *out++ = *value++;
    return out;
}
char *number(char *out, uint64_t value) {
    char digits[20];
    unsigned n = 0;
    do { digits[n++] = static_cast<char>('0' + value % 10); value /= 10; } while (value);
    while (n) *out++ = digits[--n];
    return out;
}
char *signed_number(char *out, int64_t value) {
    if (value < 0) { *out++ = '-'; value = -value; }
    return number(out, static_cast<uint64_t>(value));
}
void sink(void *, const char *line) {
    char wrapped[1280];
    char *out = text(wrapped, "MVRAW {\"pid\":");
    out = number(out, process_id);
    out = text(out, ",\"session_ms\":"); out = number(out, session_ms);
    out = text(out, ",");
    /* Every caller is our own fixed-format writer, including its closing brace. */
    out = text(out, line + 7);
    *out = 0;
    driver_log->Log(wrapped);
}
void preference_event(void *, const MvHandPreferenceEvent *event) {
    char line[256];
    char *out = text(line, "MVRAW {\"type\":\"hand_preference\",\"timestamp_ms\":");
    out = number(out, api.clock_ms());
    out = text(out, ",\"status\":\""); out = text(out, event->status);
    out = text(out, "\",\"role_hint\":"); out = number(out, event->role);
    out = text(out, ",\"matched\":"); out = number(out, event->matched);
    out = text(out, ",\"baseline\":"); out = signed_number(out, event->baseline);
    out = text(out, ",\"error\":"); out = number(out, static_cast<uint32_t>(event->error));
    out = text(out, "}"); *out = 0;
    sink(nullptr, line);
}
void headset_event(void *, const MvHeadsetEvent *event) {
    if (event->kind == MV_HEADSET_ACKNOWLEDGED && event->command == MV_HEADSET_STATUS && event->helper_ok)
        headset_ready = true;
    char line[384];
    char *out = text(line, "MVRAW {\"type\":\"headset\",\"timestamp_ms\":");
    out = number(out, api.clock_ms());
    out = text(out, ",\"event\":"); out = number(out, event->kind);
    out = text(out, ",\"command\":"); out = number(out, event->command);
    out = text(out, ",\"reason\":"); out = number(out, event->reason);
    out = text(out, ",\"sequence\":"); out = number(out, event->sequence);
    out = text(out, ",\"mode\":"); out = signed_number(out, event->mode);
    out = text(out, ",\"configuration\":"); out = signed_number(out, event->setting);
    out = text(out, ",\"helper_ok\":"); out = text(out, event->helper_ok ? "true" : "false");
    out = text(out, ",\"actual_role_verified\":false}"); *out = 0;
    sink(nullptr, line);
}
void poll_receipt(uint64_t now) {
    MvFrameReceipt receipt;
    if (!mv_frame_read_receipt(&frame, now, &receipt)) return;
    const MvHeadsetEvent event{receipt.sequence, MV_HEADSET_ACKNOWLEDGED, receipt.command,
                              MV_HEADSET_OK, static_cast<int32_t>(receipt.mode),
                              static_cast<int32_t>(receipt.setting), MV_HEADSET_UNOBSERVED, receipt.ok};
    // The first STATUS is only a challenge response, not readiness.
    if (receipt.command != MV_HEADSET_STATUS || receipt.ready) headset_event(nullptr, &event);
}
void writer_disable_and_drain() {
    _InterlockedExchange64(&writer.enabled, 0);
    const auto sleep = *reinterpret_cast<void (**)(uint32_t)>(base() + 0x7D160);
    while (!mv_writer_hook_drained(&writer)) sleep(1);
}
bool preference_restore_pending() {
    for (const auto &hand : hand_preference.hands)
        if (hand.attempted && !hand.finished) return true;
    return false;
}
void preference_restore_tick(uint64_t now) {
    if (hand_preference.enabled || (preference_restore_pending() && now >= next_preference_ms)) {
        next_preference_ms = now + 1000;
        mv_hand_preference_restore(&hand_preference);
    }
}
void automatic_stop() {
    automatic = headset_ready = false;
    requested_mode = MV_SWITCH_NONE;
    if (frame_initialized) {
        if (frame.ready) {
            const uint64_t started = api.clock_ms();
            mv_frame_shutdown_begin(&frame, started);
            while (frame.state != MV_FRAME_CLOSED && frame.state != MV_FRAME_FAILED &&
                   api.clock_ms() - started < MV_HEADSET_ACK_TIMEOUT_MS) {
                poll_receipt(api.clock_ms());
                const auto sleep = *reinterpret_cast<void (**)(uint32_t)>(base() + 0x7D160);
                sleep(10);
            }
        }
        mv_frame_transport_stop(&frame);
    }
    preference_restore_tick(api.clock_ms());
}
void automatic_start() {
    automatic_stop();
    if (preference_restore_pending()) {
        sink(nullptr, "MVRAW {\"type\":\"automatic\",\"state\":\"hand_restore_pending_requires_explicit_auto\"}");
        return;
    }
    mv_switch_reset(&worker.switching);
    if (!writer_available || !provider || !provider->mapping) {
        sink(nullptr, "MVRAW {\"type\":\"automatic\",\"state\":\"frame_writer_unavailable\"}");
        return;
    }
    writer_disable_and_drain();
    mv_frame_transport_init(&frame, provider->mapping, api.process_id(), api.clock_ms());
    frame_initialized = automatic = true;
    _InterlockedExchange64(&writer.enabled, 1);
    sink(nullptr, "MVRAW {\"type\":\"automatic\",\"state\":\"enabled\",\"transport\":\"vd-frame\"}");
}
void automatic_tick(uint64_t now) {
    if (!automatic) return;
    if (!hand_preference.enabled && preference_restore_pending()) {
        // A failed source-loss restore must not be recaptured as baseline 100.
        automatic_stop();
        sink(nullptr, "MVRAW {\"type\":\"automatic\",\"state\":\"hand_restore_pending_requires_explicit_auto\"}");
        return;
    }
    /* mv_worker_tick sampled the same identity-indexed input used here. */
    if (!worker.connected_index_devices) {
        /* Restore is a distinct loss-of-source operation, never a synthetic
         * release. It also cancels the old mode's periodic heartbeat. */
        if (requested_mode != MV_SWITCH_NONE) mv_frame_publish_intent(&frame, MV_HEADSET_RESTORE);
        requested_mode = MV_SWITCH_NONE;
        mv_switch_reset(&worker.switching);
        if (hand_preference.enabled) preference_restore_tick(now);
    } else if (headset_ready && worker.switching.wanted != MV_SWITCH_NONE &&
               (frame.desired != MV_HEADSET_RESTORE && frame.pending_command != MV_HEADSET_RESTORE)) {
        if (!hand_preference.enabled) {
            mv_hand_preference_init(&hand_preference, driver_properties, true, preference_event, nullptr);
            next_preference_ms = 0;
        }
        if (now >= next_preference_ms) {
            next_preference_ms = now + 1000;
            mv_hand_preference_apply_pending(&hand_preference);
        }
        const uint32_t wanted = worker.switching.wanted;
        if (wanted != requested_mode) {
            requested_mode = wanted;
            mv_frame_publish_intent(&frame, wanted == MV_SWITCH_INDEX ? MV_HEADSET_INDEX : MV_HEADSET_HANDS);
        }
    }
    poll_receipt(now);
    mv_frame_transport_tick(&frame, now);
    if (frame.state == MV_FRAME_FAILED) {
        const uint32_t reason = frame.reason;
        automatic_stop();
        char line[128];
        char *out = text(line, "MVRAW {\"type\":\"frame_failure\",\"reason\":");
        out = number(out, reason); out = text(out, "}"); *out = 0;
        sink(nullptr, line);
        sink(nullptr, "MVRAW {\"type\":\"automatic\",\"state\":\"failed_requires_explicit_auto\"}");
    }
}
void diagnostic(uint64_t now) {
    if (now < next_diagnostic_ms) return;
    next_diagnostic_ms = now + 1000;
    MvCaptureStats stats;
    mv_trial_capture_stats(&stats);
    const uintptr_t server = reinterpret_cast<uintptr_t>(api.module(L"vrserver.exe"));
    const auto table = *reinterpret_cast<uintptr_t **>(driver_input);
    char line[768];
    char *out = text(line, "MVRAW {\"type\":\"capture\",\"timestamp_ms\":");
    out = number(out, now);
    out = text(out, ",\"create_calls\":"); out = number(out, stats.create_calls);
    out = text(out, ",\"update_calls\":"); out = number(out, stats.update_calls);
    out = text(out, ",\"create_failures\":"); out = number(out, stats.create_failures);
    out = text(out, ",\"update_failures\":"); out = number(out, stats.update_failures);
    out = text(out, ",\"trackpad_creates\":"); out = number(out, stats.trackpad_creates);
    out = text(out, ",\"first_callback_ms\":"); out = number(out, stats.first_callback_ms);
    out = text(out, ",\"public004_create_calls\":"); out = number(out, stats.public_create_calls);
    out = text(out, ",\"public004_update_calls\":"); out = number(out, stats.public_update_calls);
    out = text(out, ",\"public_create_rva\":"); out = number(out, server ? table[0] - server : 0);
    out = text(out, ",\"public_update_rva\":"); out = number(out, server ? table[1] - server : 0);
    out = text(out, ",\"expected_public_targets\":");
    out = text(out, server && table[0] == server + 0x263D0 && table[1] == server + 0x27B00 ? "true" : "false");
    out = text(out, ",\"trampolines_initialized\":");
    out = text(out, *reinterpret_cast<uintptr_t *>(base() + 0x9A7B8) &&
                    *reinterpret_cast<uintptr_t *>(base() + 0x9A7C0) ? "true" : "false");
    out = text(out, "}"); *out = 0;
    sink(nullptr, line);
}
}

extern "C" int mv_diag_prepare(void *context, const MvNativeApi *system) {
    api = *system;
    process_id = api.process_id();
    session_ms = api.clock_ms();
    auto *driver_context = static_cast<vr::IVRDriverContext *>(context);
    vr::EVRInitError error = vr::VRInitError_None;
    driver_log = static_cast<vr::IVRDriverLog *>(driver_context->GetGenericInterface("IVRDriverLog_001", &error));
    if (!driver_log) return 0;
    auto *properties = static_cast<vr::IVRProperties *>(driver_context->GetGenericInterface("IVRProperties_001", &error));
    auto *host = static_cast<vr::IVRServerDriverHost *>(driver_context->GetGenericInterface("IVRServerDriverHost_006", &error));
    driver_input = static_cast<vr::IVRDriverInput *>(driver_context->GetGenericInterface("IVRDriverInput_004", &error));
    if (!properties || !host || !driver_input) {
        sink(nullptr, "MVRAW {\"type\":\"lifecycle\",\"state\":\"interfaces_unavailable\"}");
        return 0;
    }
    driver_properties = properties;
    automatic = headset_ready = phase_started = false;
    requested_mode = MV_SWITCH_NONE;
    const auto table = *reinterpret_cast<uintptr_t **>(driver_input);
    mv_trial_set_targets(table[0], table[1]);
    const MvWorkerBindings bindings{mv_trial_probe(), properties, host, sink, clock_ms, nullptr, nullptr};
    mv_worker_init(&worker, &bindings);
    mv_hand_preference_init(&hand_preference, properties, false, preference_event, nullptr);
    next_diagnostic_ms = next_preference_ms = 0;
    sink(nullptr, "MVRAW {\"type\":\"lifecycle\",\"state\":\"prepared\",\"automatic_switching\":true,\"automatic_enabled\":false,\"historical_backfill\":false,\"requires_fresh_create\":true,\"fixed_hand_priority\":100,\"actual_role_query\":false}");
    return 1;
}

extern "C" void mv_diag_tick(uint64_t now, uint64_t sequence, uint32_t phase) {
    const bool default_auto = !phase_started && !sequence && phase == MV_WORKER_AUTO;
    phase_started = true;
    const uint64_t previous = worker.phase_id;
    mv_worker_set_phase(&worker, sequence, static_cast<MvWorkerPhase>(phase));
    if (default_auto) {
        mv_mark_phase(worker.bindings.probe);
        worker.phase = MV_WORKER_AUTO;
        automatic_start();
    } else if (worker.phase_id != previous) {
        if (worker.phase == MV_WORKER_AUTO) automatic_start();
        else if (worker.phase == MV_WORKER_RESTORE || worker.phase == MV_WORKER_STOP) automatic_stop();
    }
    if (!worker.exit_requested && (automatic || worker.phase == MV_WORKER_INDEX || worker.phase == MV_WORKER_HANDS)) {
        diagnostic(now);
        mv_worker_tick(&worker);
        automatic_tick(api.clock_ms());
    }
    // Keep the old capture until recovery completes. Recovery never rearms auto.
    if (!hand_preference.enabled && preference_restore_pending()) preference_restore_tick(api.clock_ms());
}

extern "C" void mv_diag_shutdown(void) {
    automatic_stop();
    // SDK teardown is next: do not let the normal retry cadence skip the
    // final recovery opportunity while the property interfaces still exist.
    if (preference_restore_pending()) mv_hand_preference_restore(&hand_preference);
    sink(nullptr, "MVRAW {\"type\":\"lifecycle\",\"state\":\"stopped\"}");
}

extern "C" __declspec(dllexport)
int32_t mv_trial_init(void *self, void *context) {
    using Original = int32_t (*)(void *, void *);
    const int32_t result = reinterpret_cast<Original>(base() + 0x119D0)(self, context);
    if (!result) {
        MvNativeApi bindings;
        mv_native_bind(&bindings, base());
        provider = static_cast<VdNativeProvider *>(self);
        api = bindings;
        if (provider->mapping) {
            mv_frame_transport_init(&frame, provider->mapping, api.process_id(), api.clock_ms());
            frame_initialized = true;
            // This new provider has not been returned to SteamVR. A claimed
            // renderer is not treated as a safe first-install window.
            MvWriterApi patch_api;
            mv_writer_api_bind(&patch_api, base(), &bindings);
            if (writer.installed) {
                writer_disable_and_drain();
                writer_available = true;
            } else if (frame.state == MV_FRAME_WAIT_SOURCE &&
                       patch_api.module_ref(5, reinterpret_cast<const wchar_t *>(&mv_trial_init), &driver_reference)) {
                const uint32_t installed = mv_writer_hook_start(&writer, &frame, &bindings, base(), 1);
                writer_available = installed == MV_WRITER_OK;
                // Retain this capsule's DLL while the runtime points into it.
                // Disabled passthrough survives Cleanup until process exit.
                // GET_MODULE_HANDLE_EX_FLAG_PIN keeps this code resident until
                // process exit, even if installation leaves a disabled hook.
            }
            mv_frame_transport_stop(&frame);
        }
        if (config_paths(bindings)) mv_runtime_start(&bindings, phase_path, context);
    }
    return result;
}

extern "C" __declspec(dllexport)
void mv_trial_cleanup(void *self) {
    mv_runtime_stop();
    mv_writer_hook_stop(&writer, 0);
    if (writer.installed) writer_disable_and_drain();
    frame_initialized = writer_available = false;
    provider = nullptr;
    using Original = void (*)(void *);
    reinterpret_cast<Original>(base() + 0x11800)(self);
}
