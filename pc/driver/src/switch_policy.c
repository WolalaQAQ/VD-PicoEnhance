#include "switch_policy.h"

void mv_switch_reset(MvSwitchPolicy *p) { *p = (MvSwitchPolicy){0}; }

uint32_t mv_switch_tick(MvSwitchPolicy *p, const MvSwitchInput inputs[2], uint64_t now) {
    int pickup = 0, released = 1, present = 0;
    for (uint32_t side = 0; side < 2; ++side) {
        const MvSwitchInput *in = &inputs[side];
        const MvSample *s = &in->sample;
        MvSwitchSide *state = &p->sides[side];
        if (!in->present) { *state = (MvSwitchSide){0}; continue; }
        present = 1;
        if (state->container != in->container || state->generation != in->generation || state->index != in->index) {
            *state = (MvSwitchSide){0};
            state->container = in->container;
            state->generation = in->generation;
            state->index = in->index;
        }
        const int identity = in->container && in->generation;
        /* Never join one controller's touch with the other's motion. */
        if (identity && s->known && s->touched && s->pose_known && s->moving) pickup = 1;
        const int idle = identity && s->known && !s->touched && s->pose_known && s->stationary;
        if (!idle || (state->idle && now < state->idle_since)) state->idle = 0;
        if (idle && !state->idle) { state->idle_since = now; state->idle = 1; }
        if (!state->idle || now - state->idle_since < 1000) released = 0;
    }
    if (!present) mv_switch_reset(p);
    else if (pickup) p->wanted = MV_SWITCH_INDEX;
    else if (released) p->wanted = MV_SWITCH_HANDS;
    return p->wanted;
}
