#include "controller.h"

bool mv_standalone_restore_pending(const MvStandalone *s) {
    for (const auto &hand : s->preference.hands)
        if (hand.attempted && !hand.finished) return true;
    return false;
}

static void restore_preference(MvStandalone *s, uint64_t now) {
    if (s->preference.enabled || (mv_standalone_restore_pending(s) && now >= s->next_preference)) {
        s->next_preference = now + 1000;
        mv_hand_preference_restore(&s->preference);
    }
}

void mv_standalone_init(MvStandalone *s, const MvWorkerBindings *io, MvHandPreferenceSink preference_sink) {
    *s = {};
    mv_worker_init(&s->worker, io);
    mv_hand_preference_init(&s->preference, io->properties, false, preference_sink, io->user);
}

bool mv_standalone_start(MvStandalone *s, const MvHeadsetBindings *io, uint16_t port, const char *token) {
    if (mv_standalone_restore_pending(s) ||
        (s->client.state >= MV_HEADSET_IDLE && s->client.state <= MV_HEADSET_ONLINE)) return false;
    if (!mv_headset_init(&s->client, io, port, token, nullptr)) return false;
    mv_switch_reset(&s->worker.switching);
    mv_mark_phase(s->worker.bindings.probe);
    s->worker.phase = MV_WORKER_AUTO;
    s->automatic = true;
    s->ready = false;
    s->requested = MV_SWITCH_NONE;
    s->next_status = s->next_preference = 0;
    return mv_headset_request(&s->client, MV_HEADSET_STATUS) != 0;
}

void mv_standalone_event(MvStandalone *s, const MvHeadsetEvent *e) {
    if (e->kind == MV_HEADSET_ACKNOWLEDGED && e->command == MV_HEADSET_STATUS && e->helper_ok)
        s->ready = true;
    if (e->kind == MV_HEADSET_DISCONNECTED) s->ready = false;
}

void mv_standalone_stop(MvStandalone *s, uint64_t now) {
    s->automatic = s->ready = false;
    s->requested = MV_SWITCH_NONE;
    mv_switch_reset(&s->worker.switching);
    mv_headset_shutdown_begin(&s->client, now);
    restore_preference(s, now);
}

void mv_standalone_tick(MvStandalone *s, uint64_t now) {
    mv_headset_tick(&s->client, now);
    if (s->automatic && s->client.state == MV_HEADSET_FAILED) {
        s->automatic = s->ready = false;
        s->requested = MV_SWITCH_NONE;
        mv_switch_reset(&s->worker.switching);
        restore_preference(s, now);
        s->worker.bindings.log_sink(s->worker.bindings.user,
            "MVRAW {\"type\":\"automatic\",\"state\":\"connection_failed_restoring\"}");
    }
    if (s->automatic && !s->preference.enabled && mv_standalone_restore_pending(s)) {
        mv_standalone_stop(s, now);
        s->worker.bindings.log_sink(s->worker.bindings.user,
            "MVRAW {\"type\":\"automatic\",\"state\":\"hand_restore_pending_requires_explicit_auto\"}");
    }
    if (s->automatic) {
        mv_worker_tick(&s->worker);
        if (!s->worker.connected_index_devices) {
            if (s->requested != MV_SWITCH_NONE) mv_headset_request(&s->client, MV_HEADSET_RESTORE);
            s->requested = MV_SWITCH_NONE;
            mv_switch_reset(&s->worker.switching);
            if (s->preference.enabled) restore_preference(s, now);
        } else if (s->ready && s->worker.switching.wanted != MV_SWITCH_NONE &&
                   s->client.pending_command != MV_HEADSET_RESTORE) {
            if (!s->preference.enabled) {
                mv_hand_preference_init(&s->preference, s->worker.bindings.properties, true,
                    s->preference.sink, s->preference.user);
                s->next_preference = 0;
            }
            if (now >= s->next_preference) {
                s->next_preference = now + 1000;
                mv_hand_preference_apply_pending(&s->preference);
            }
            const auto wanted = s->worker.switching.wanted;
            if (wanted != s->requested) {
                s->requested = wanted;
                mv_headset_request(&s->client, wanted == MV_SWITCH_INDEX ? MV_HEADSET_INDEX : MV_HEADSET_HANDS);
            }
        }
        // Keep the single connection alive while waiting for controllers, and
        // after source-loss Restore. STATUS never renews a system mode lease.
        if (s->automatic && now >= s->next_status && !s->client.pending_command &&
            s->client.desired != MV_HEADSET_INDEX && s->client.desired != MV_HEADSET_HANDS) {
            s->next_status = now + MV_HEADSET_HEARTBEAT_MS;
            mv_headset_request(&s->client, MV_HEADSET_STATUS);
        }
        mv_headset_tick(&s->client, now);
    }
    if (!s->preference.enabled && mv_standalone_restore_pending(s)) restore_preference(s, now);
}
