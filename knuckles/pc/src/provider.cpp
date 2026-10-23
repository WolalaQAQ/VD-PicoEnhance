#include "lan_transport.h"
#include "controller.h"
#include "input_capture.h"
#include "phase_control.h"
#include <windows.h>
#include <cstdio>
#include <cstring>

namespace {
class Provider final : public vr::IServerTrackedDeviceProvider {
public:
    vr::EVRInitError Init(vr::IVRDriverContext *context) override {
        vr::EVRInitError error = vr::VRInitError_None;
        log_ = static_cast<vr::IVRDriverLog *>(context->GetGenericInterface(vr::IVRDriverLog_Version, &error));
        properties_ = static_cast<vr::IVRProperties *>(context->GetGenericInterface(vr::IVRProperties_Version, &error));
        host_ = static_cast<vr::IVRServerDriverHost *>(context->GetGenericInterface(vr::IVRServerDriverHost_Version, &error));
        if (!log_ || !properties_ || !host_) return vr::VRInitError_Driver_Failed;
        if (!mv_capture_start(context)) {
            log_->Log("PicoHand<->Knuckles: input capture unavailable; no automatic control");
            return vr::VRInitError_Driver_Failed;
        }
        const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", directory_, 900);
        if (!n || n >= 900) { mv_capture_stop(); return vr::VRInitError_Driver_Failed; }
        wcscat(directory_, L"\\PicoMixedVR\\");
        const MvWorkerBindings bindings{mv_capture_probe(), properties_, host_, log_line, clock_ms, nullptr, this};
        mv_standalone_init(&state_, &bindings, preference_event);
        pid_ = GetCurrentProcessId(); session_ = GetTickCount64();
        sequence_ = 0; phase_ = MV_WORKER_RESTORE;
        armed_ = discovering_ = false; retryable_ = true; next_connect_ = 0;
        InterlockedExchange(&standby_, 0);
        wake_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (wake_) thread_ = CreateThread(nullptr, 0, run, this, 0, nullptr);
        if (!thread_) {
            if (wake_) CloseHandle(wake_);
            wake_ = nullptr;
            mv_capture_stop();
            return vr::VRInitError_Driver_Failed;
        }
        log_line(this, "MVRAW {\"type\":\"lifecycle\",\"state\":\"prepared\",\"automatic_switching\":true,\"automatic_enabled\":false,\"transport\":\"lan\",\"requires_fresh_create\":true}");
        log_->Log("PicoHand<->Knuckles: provider ready; no devices added; saved control only");
        return vr::VRInitError_None;
    }
    void Cleanup() override {
        if (thread_) {
            SetEvent(wake_);
            WaitForSingleObject(thread_, INFINITE);
            CloseHandle(thread_); CloseHandle(wake_);
            thread_ = wake_ = nullptr;
        }
        mv_capture_stop();
        properties_ = nullptr; host_ = nullptr; log_ = nullptr;
    }
    const char *const *GetInterfaceVersions() override { return vr::k_InterfaceVersions; }
    void RunFrame() override {}
    bool ShouldBlockStandbyMode() override { return false; }
    void EnterStandby() override { InterlockedExchange(&standby_, 1); }
    void LeaveStandby() override { InterlockedExchange(&standby_, 0); }
private:
    static uint64_t clock_ms(void *) { return GetTickCount64(); }
    static void log_line(void *user, const char *line) {
        auto *self = static_cast<Provider *>(user);
        char wrapped[1280];
        snprintf(wrapped, sizeof(wrapped), "MVRAW {\"pid\":%lu,\"session_ms\":%llu,\"standalone\":true,%s",
                 static_cast<unsigned long>(self->pid_), static_cast<unsigned long long>(self->session_), line + 7);
        self->log_->Log(wrapped);
    }
    static void preference_event(void *user, const MvHandPreferenceEvent *e) {
        char line[256];
        snprintf(line, sizeof(line), "MVRAW {\"type\":\"hand_preference\",\"status\":\"%s\",\"role_hint\":%u,\"baseline\":%d,\"error\":%d}",
                 e->status, e->role, e->baseline, static_cast<int>(e->error));
        log_line(user, line);
    }
    static void headset_event(void *user, const MvHeadsetEvent *e) {
        auto *self = static_cast<Provider *>(user);
        mv_standalone_event(&self->state_, e);
        if (e->kind == MV_HEADSET_DISCONNECTED) {
            self->retryable_ = e->reason == MV_HEADSET_CONNECT_ERROR || e->reason == MV_HEADSET_CONNECT_TIMEOUT ||
                e->reason == MV_HEADSET_SEND_ERROR || e->reason == MV_HEADSET_RECEIVE_ERROR ||
                e->reason == MV_HEADSET_PEER_CLOSED || e->reason == MV_HEADSET_ACK_TIMEOUT ||
                e->reason == MV_HEADSET_PROCESS_PAUSE;
            self->next_connect_ = GetTickCount64() + 2000;
        }
        char line[320];
        snprintf(line, sizeof(line), "MVRAW {\"type\":\"headset\",\"event\":%u,\"command\":%u,\"sequence\":%llu,\"reason\":%u,\"mode\":%d,\"configuration\":%d,\"helper_ok\":%s}",
                 e->kind, e->command, static_cast<unsigned long long>(e->sequence), e->reason,
                 e->mode, e->setting, e->helper_ok ? "true" : "false");
        log_line(user, line);
    }
    uint32_t read_file(const wchar_t *name, char *data, uint32_t capacity) {
        wchar_t path[1024];
        wcscpy(path, directory_); wcscat(path, name);
        HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return 0;
        DWORD bytes = 0;
        const BOOL ok = ReadFile(file, data, capacity, &bytes, nullptr);
        CloseHandle(file);
        return ok && bytes < capacity ? bytes : 0;
    }
    bool start(uint64_t now) {
        if (mv_standalone_restore_pending(&state_)) return false;
        char endpoint[64]{}, token[68]{};
        uint32_t n = read_file(L"endpoint.txt", endpoint, sizeof(endpoint));
        while (n && (endpoint[n - 1] == '\r' || endpoint[n - 1] == '\n')) endpoint[--n] = 0;
        if (!n) return false;
        n = read_file(L"headset.token", token, sizeof(token));
        const bool valid = n == 64 || (n == 65 && token[64] == '\n') ||
            (n == 66 && token[64] == '\r' && token[65] == '\n');
        if (!valid) { SecureZeroMemory(token, sizeof(token)); return false; }
        mv_lan_cleanup(&transport_);
        bool ok = mv_lan_prepare(&transport_, endpoint, headset_event, this);
        if (ok) {
            memcpy(token_, token, 64);
            ok = mv_lan_discover_begin(&transport_, token_, now);
            discovering_ = ok;
        }
        SecureZeroMemory(token, sizeof(token));
        if (!ok) mv_lan_cleanup(&transport_);
        return ok;
    }
    void connect_tick(uint64_t now) {
        if (!armed_ || !retryable_ || InterlockedCompareExchange(&standby_, 0, 0) ||
            mv_standalone_restore_pending(&state_)) return;
        if (discovering_) {
            const int found = mv_lan_discover_poll(&transport_, now);
            if (!found) return;
            discovering_ = false;
            // No response also permits connecting to the saved address, for
            // compatibility with already installed pre-discovery daemons.
            const auto bindings = mv_lan_bindings(&transport_);
            const bool ok = mv_standalone_start(&state_, &bindings, ntohs(transport_.peer.sin_port), token_);
            log_line(this, found > 0 ? "MVRAW {\"type\":\"discovery\",\"state\":\"paired_peer_found\"}" :
                "MVRAW {\"type\":\"discovery\",\"state\":\"saved_address\"}");
            log_line(this, ok ? "MVRAW {\"type\":\"automatic\",\"state\":\"enabled\",\"transport\":\"lan\"}" :
                "MVRAW {\"type\":\"automatic\",\"state\":\"configuration_or_restore_not_ready\"}");
            next_connect_ = now + 2000;
            SecureZeroMemory(token_, sizeof(token_));
        } else if (!state_.automatic &&
                   !(state_.client.state >= MV_HEADSET_IDLE && state_.client.state <= MV_HEADSET_ONLINE) &&
                   now >= next_connect_) {
            next_connect_ = now + 2000;
            if (start(now)) log_line(this, "MVRAW {\"type\":\"discovery\",\"state\":\"searching\"}");
        }
    }
    void control(uint64_t now) {
        char text[64];
        const uint32_t bytes = read_file(L"phase.txt", text, sizeof(text));
        uint64_t sequence;
        uint32_t phase;
        if (!bytes || !mv_parse_phase(text, bytes, &sequence, &phase) || sequence <= sequence_) return;
        sequence_ = sequence; phase_ = phase;
        state_.worker.phase_id = sequence;
        state_.worker.phase = static_cast<MvWorkerPhase>(phase);
        armed_ = phase == MV_WORKER_AUTO;
        if (armed_) {
            retryable_ = true; next_connect_ = now;
            // A saved auto choice permits rediscovery; explicit Stop cancels it.
            if (state_.client.state >= MV_HEADSET_IDLE && state_.client.state <= MV_HEADSET_ONLINE) {
                log_line(this, "MVRAW {\"type\":\"automatic\",\"state\":\"already_running_or_restore_pending\"}");
                return;
            }
        } else if (phase == MV_WORKER_RESTORE || phase == MV_WORKER_STOP) {
            discovering_ = false;
            mv_lan_discover_cancel(&transport_);
            SecureZeroMemory(token_, sizeof(token_));
            mv_standalone_stop(&state_, now);
        }
    }
    static DWORD WINAPI run(void *user) { static_cast<Provider *>(user)->loop(); return 0; }
    void loop() {
        bool was_standby = false;
        while (WaitForSingleObject(wake_, 20) == WAIT_TIMEOUT) {
            const uint64_t now = GetTickCount64();
            const bool standby = InterlockedCompareExchange(&standby_, 0, 0) != 0;
            if (standby && !was_standby) {
                discovering_ = false; mv_lan_discover_cancel(&transport_);
                SecureZeroMemory(token_, sizeof(token_));
                mv_standalone_stop(&state_, now);
            }
            was_standby = standby;
            control(now);
            connect_tick(now);
            const bool automatic = state_.automatic;
            mv_standalone_tick(&state_, now);
            // An unfinished HAND recovery is not a transient network failure.
            if (automatic && !state_.automatic && mv_standalone_restore_pending(&state_)) retryable_ = false;
        }
        mv_lan_discover_cancel(&transport_);
        SecureZeroMemory(token_, sizeof(token_));
        mv_standalone_stop(&state_, GetTickCount64());
        const auto until = GetTickCount64() + MV_HEADSET_ACK_TIMEOUT_MS;
        while (state_.client.state >= MV_HEADSET_IDLE && state_.client.state <= MV_HEADSET_ONLINE && GetTickCount64() < until) {
            mv_standalone_tick(&state_, GetTickCount64());
            Sleep(10);
        }
        mv_headset_close(&state_.client);
        if (mv_standalone_restore_pending(&state_)) mv_hand_preference_restore(&state_.preference);
        mv_lan_cleanup(&transport_);
        log_line(this, "MVRAW {\"type\":\"lifecycle\",\"state\":\"stopped\"}");
    }
    MvStandalone state_{};
    MvLanTransport transport_{};
    vr::IVRProperties *properties_ = nullptr;
    vr::IVRServerDriverHost *host_ = nullptr;
    vr::IVRDriverLog *log_ = nullptr;
    HANDLE wake_ = nullptr, thread_ = nullptr;
    volatile LONG standby_ = 0;
    DWORD pid_ = 0;
    uint64_t session_ = 0, sequence_ = 0;
    uint32_t phase_ = MV_WORKER_RESTORE;
    bool armed_ = false, discovering_ = false, retryable_ = true;
    uint64_t next_connect_ = 0;
    char token_[64]{};
    wchar_t directory_[1024]{};
};
Provider provider;
}
extern "C" __declspec(dllexport) void *HmdDriverFactory(const char *name, int *error) {
    if (name && !strcmp(name, vr::IServerTrackedDeviceProvider_Version)) return &provider;
    if (error) *error = vr::VRInitError_Init_InterfaceNotFound;
    return nullptr;
}
