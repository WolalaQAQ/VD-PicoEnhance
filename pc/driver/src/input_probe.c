#include "input_probe.h"
#include <stddef.h>
#include <intrin.h>

static void enter(MvProbe *p) {
    while (_InterlockedCompareExchange(&p->lock, 1, 0)) _mm_pause();
}
static void leave(MvProbe *p) { _InterlockedExchange(&p->lock, 0); }

static int equal(const char *a, const char *b) {
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}

void mv_reset(MvProbe *p) {
    uint8_t *bytes = (uint8_t *)p;
    for (size_t i = 0; i < sizeof(*p); ++i) bytes[i] = 0;
}

void mv_mark_phase(MvProbe *p) {
    enter(p);
    ++p->phase;
    for (size_t i = 0; i < MV_MAX_COMPONENTS; ++i)
        p->components[i].phase_baseline = p->components[i].updates;
    leave(p);
}

void mv_created(MvProbe *p, uint64_t container, const char *path,
                uint64_t component, int32_t result) {
    if (result || !container || !component || !path || !equal(path, MV_TRACKPAD_TOUCH)) return;
    enter(p);
    MvComponent *entry = NULL, *empty = NULL;
    for (size_t i = 0; i < MV_MAX_COMPONENTS; ++i) {
        MvComponent *e = &p->components[i];
        if (e->component == component || e->container == container) { entry = e; break; }
        if (!e->component && !empty) empty = e;
    }
    if (!entry) entry = empty;
    if (!entry) ++p->overflow;
    else if (entry->component != component || entry->container != container) {
        *entry = (MvComponent){0};
        entry->container = container;
        entry->component = component;
    }
    leave(p);
}

void mv_updated(MvProbe *p, uint64_t component, uint8_t value, int32_t result, uint64_t now) {
    if (result || !component) return;
    enter(p);
    for (size_t i = 0; i < MV_MAX_COMPONENTS; ++i) {
        MvComponent *e = &p->components[i];
        if (e->component != component) continue;
        e->known = 1;
        e->touched = value != 0;
        e->last_update_ms = now;
        ++e->updates;
        break;
    }
    leave(p);
}

static int finite_velocity(const double v[3]) {
    /* Explicit finite bound also rejects +/-Inf without a CRT dependency. */
    const double maximum = 1.7976931348623157e308;
    for (size_t i = 0; i < 3; ++i)
        if (!(v[i] >= -maximum && v[i] <= maximum)) return 0;
    return 1;
}

static double length2(const double v[3]) {
    return v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
}

int mv_sample(MvProbe *p, const MvIdentity *id, const MvPose *pose, MvSample *out) {
    *out = (MvSample){0};
    if (!id->real_index || !id->container || !id->generation || id->device_index >= 64 ||
        (id->role != 1 && id->role != 2)) return 0;
    enter(p);
    int found = 0;
    for (size_t i = 0; i < MV_MAX_COMPONENTS; ++i) {
        MvComponent *e = &p->components[i];
        if (!e->component || e->container != id->container) continue;
        if ((e->identity_bound && (e->identity_generation != id->generation ||
                                  e->device_index != id->device_index)) || !pose->connected) {
            e->known = 0;
            e->phase_baseline = e->updates;
        }
        e->identity_bound = 1;
        e->device_index = id->device_index;
        e->identity_generation = id->generation;
        out->phase = p->phase;
        out->updates = e->updates;
        out->updates_in_phase = e->updates - e->phase_baseline;
        out->last_update_ms = e->last_update_ms;
        out->device_index = id->device_index;
        out->role = id->role;
        out->known = e->known;
        out->touched = e->known && e->touched;
        out->observed_this_phase = e->known && out->updates_in_phase != 0;
        out->pose_known = pose->connected && pose->valid && pose->tracking_result == 200 &&
            finite_velocity(pose->velocity) && finite_velocity(pose->angular_velocity);
        out->moving = out->pose_known &&
            (length2(pose->velocity) > (double)0.02f * (double)0.02f ||
             length2(pose->angular_velocity) > (double)0.15f * (double)0.15f);
        /* Release has a wider quiet band: a resting Knuckles can rock on its
         * curved body. Pickup keeps the original touch AND motion thresholds. */
        out->stationary = out->pose_known &&
            length2(pose->velocity) <= (double)0.04f * (double)0.04f &&
            length2(pose->angular_velocity) <= (double)0.4f * (double)0.4f;
        out->index_candidate = out->observed_this_phase && out->touched && out->moving;
        found = 1;
        break;
    }
    leave(p);
    return found;
}
