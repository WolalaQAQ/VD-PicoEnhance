#ifndef MIXEDVR_TRIAL_WORKER_H
#define MIXEDVR_TRIAL_WORKER_H
#include "sdk_sampler.h"
#include "switch_policy.h"

enum MvWorkerPhase {
    MV_WORKER_UNKNOWN = 0,
    MV_WORKER_INDEX = 1,
    MV_WORKER_HANDS = 2,
    MV_WORKER_STOP = 3,
    MV_WORKER_AUTO = 4,
    MV_WORKER_RESTORE = 5
};
enum { MV_WORKER_INTERVAL_MS = 100, MV_WORKER_RECOVER_MS = 1000,
       MV_WORKER_LINE_BYTES = 1024 };

using MvWorkerSink = void (*)(void *user, const char *line);
using MvWorkerClock = uint64_t (*)(void *user);
using MvWorkerRecover = int (*)(void *user, uint64_t container, uint64_t *component);

struct MvWorkerBindings {
    MvProbe *probe;
    vr::IVRProperties *properties;
    vr::IVRServerDriverHost *host;
    MvWorkerSink log_sink;
    MvWorkerClock clock_ms;
    MvWorkerRecover recover; /* Optional read-only registry lookup. No cached bool. */
    void *user;
};

/* Persistent private memory, never serialize/log this structure.
 * Initialize before making the context reachable. Phase changes and ticks
 * are serialized on the independent worker thread, never RunFrame/input
 * callbacks. Sink/clock must be valid; sink
 * consumes the line synchronously without re-entering this worker.
 * Input callbacks may concurrently update bindings.probe via the C core. */
struct MvWorkerContext {
    MvWorkerBindings bindings;
    MvSdkSampler sdk;
    MvSample samples[MV_SDK_DEVICE_COUNT];
    MvSwitchPolicy switching;
    uint64_t phase_id, next_sample_ms;
    uint64_t recover_after_ms[MV_SDK_DEVICE_COUNT];
    uint32_t connected_index_devices;
    MvWorkerPhase phase;
    bool sampled, exit_requested;
    char line[MV_WORKER_LINE_BYTES];
};

extern "C" {
/* Does not reset the caller-owned probe or create a thread. Phase 0=unknown. */
void mv_worker_init(MvWorkerContext *context, const MvWorkerBindings *bindings);
/* Monotonic sequence: duplicate/stale markers are ignored. Accepted markers
 * call mv_mark_phase once; STOP requests exit and permanently silences ticks.
 * index/hands remain diagnostic labels. entry.cpp handles explicit auto/restore
 * markers separately; this sampler itself never sends commands. */
void mv_worker_set_phase(MvWorkerContext *context, uint64_t sequence, MvWorkerPhase phase);
/* Non-blocking cadence check; at most once/100ms sample all 64 raw poses.
 * Returns emitted line count: <=2 verified devices (one/role hint)+1 summary.
 * Missing mappings may recover at most once/device/second, after sampling;
 * recovered mappings are first sampled on a subsequent tick. */
uint32_t mv_worker_tick(MvWorkerContext *context);
}
#endif
