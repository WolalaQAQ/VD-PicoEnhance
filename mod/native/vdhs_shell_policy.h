#ifndef VDHS_SHELL_POLICY_H
#define VDHS_SHELL_POLICY_H

// PICO's private XrServerDisplayState, not the OpenXR XrSessionState enum.
// PR #1 identifies 6 as the state used by the Shell's existing skip rule.
static inline int vdhs_shell_disable_for_vd(int display_state, int is_self) {
    return display_state == 6 && !is_self;
}

#endif
