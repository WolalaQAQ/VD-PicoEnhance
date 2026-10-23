#ifndef MV_SWITCH_POLICY_H
#define MV_SWITCH_POLICY_H
#include "input_probe.h"

enum { MV_SWITCH_NONE = 0, MV_SWITCH_INDEX = 1, MV_SWITCH_HANDS = 2 };
typedef struct {
    uint64_t container, generation;
    uint32_t index, present;
    MvSample sample;
} MvSwitchInput;
typedef struct {
    uint64_t container, generation, idle_since;
    uint32_t index;
    uint8_t idle;
} MvSwitchSide;
typedef struct {
    MvSwitchSide sides[2];
    uint32_t wanted;
} MvSwitchPolicy;

#ifdef __cplusplus
extern "C" {
#endif
void mv_switch_reset(MvSwitchPolicy *policy);
/* Port of a13426a main.cpp auto decision and index_input.cpp last-use logic.
 * Inputs are already verified real Index identities, not selected hand roles.
 * Unknown/invalid pose breaks the continuous released+stationary interval.
 * Absent sides are not release samples; every present side must be explicitly
 * released for 1000ms. No present devices resets the decision to NONE. */
uint32_t mv_switch_tick(MvSwitchPolicy *policy, const MvSwitchInput inputs[2], uint64_t now);
#ifdef __cplusplus
}
#endif
#endif
