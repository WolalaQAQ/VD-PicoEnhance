#pragma once
#include "worker.h"
#include "hand_preference.h"
#include "headset_client.h"

struct MvStandalone {
    MvWorkerContext worker;
    MvHandPreference preference;
    MvHeadsetClient client;
    uint64_t next_status, next_preference;
    uint32_t requested;
    bool automatic, ready;
};

void mv_standalone_init(MvStandalone *, const MvWorkerBindings *, MvHandPreferenceSink);
bool mv_standalone_start(MvStandalone *, const MvHeadsetBindings *, uint16_t port, const char *token);
void mv_standalone_event(MvStandalone *, const MvHeadsetEvent *);
void mv_standalone_stop(MvStandalone *, uint64_t now);
void mv_standalone_tick(MvStandalone *, uint64_t now);
bool mv_standalone_restore_pending(const MvStandalone *);
