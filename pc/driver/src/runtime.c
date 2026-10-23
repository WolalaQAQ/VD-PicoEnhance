#include "runtime.h"
#include "phase_control.h"

typedef struct {
    MvNativeApi api;
    const wchar_t *phase_path;
    void *thread, *stop;
    uint64_t sequence;
    uint32_t phase;
} MvRuntime;

static MvRuntime runtime;

static void phase_file(MvRuntime *r) {
    void *file = r->api.open_file(r->phase_path, 0x80000000u, 7, 0, 3, 0x80, 0);
    if (file == (void *)(intptr_t)-1) {
        const uint32_t error = r->api.last_error();
        /* A new profile defaults to auto. An unreadable saved choice is not
         * treated as absent; existing stop/restore markers still take priority. */
        if (!r->sequence && !r->phase && (error == 2 || error == 3)) r->phase = 4;
        return;
    }
    char text[64];
    uint32_t bytes = 0;
    int32_t success = r->api.read_file(file, text, sizeof(text), &bytes, 0);
    r->api.close(file);
    uint64_t sequence;
    uint32_t phase;
    if (success && bytes < sizeof(text) && mv_parse_phase(text, bytes, &sequence, &phase) &&
        sequence > r->sequence) {
        r->sequence = sequence;
        r->phase = phase;
    }
}

uint32_t mv_runtime_thread(void *unused) {
    (void)unused;
    MvRuntime *r = &runtime;
    do {
        phase_file(r);
        mv_diag_tick(r->api.clock_ms(), r->sequence, r->phase);
        if (r->phase == 3) break;
    } while (r->api.wait(r->stop, 100) == 258);
    mv_diag_shutdown();
    return 0;
}

int mv_runtime_start(const MvNativeApi *api, const wchar_t *phase_path, void *context) {
    MvRuntime *r = &runtime;
    r->api = *api;
    r->phase_path = phase_path;
    r->sequence = 0;
    r->phase = 0;
    if (!mv_diag_prepare(context, api)) return 0;
    /* CreateEventExW: manual reset, initially non-signalled; unnamed. */
    r->stop = api->create_event(0, 0, 1, 0x1F0003);
    if (!r->stop) {
        mv_diag_shutdown();
        return 0;
    }
    r->thread = api->create_thread(0, 0, mv_runtime_thread, 0, 0, 0);
    if (!r->thread) {
        api->close(r->stop);
        r->stop = 0;
        mv_diag_shutdown();
        return 0;
    }
    return 1;
}

void mv_runtime_stop(void) {
    MvRuntime *r = &runtime;
    if (!r->thread) return;
    r->api.set_event(r->stop);
    /* Cleanup joins the diagnostic worker before original SDK teardown. */
    r->api.wait(r->thread, UINT32_MAX);
    r->api.close(r->thread);
    r->api.close(r->stop);
    r->thread = r->stop = 0;
}
