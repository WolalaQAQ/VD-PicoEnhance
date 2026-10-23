#ifndef MV_TRIAL_HAND_PREFERENCE_H
#define MV_TRIAL_HAND_PREFERENCE_H
#include <openvr_driver.h>

struct MvHandPreferenceEvent {
    const char *status; /* Fixed strings only; no serial or device path. */
    uint32_t role, matched;
    int32_t baseline;
    vr::ETrackedPropertyError error;
};
/* Sink consumes the stack event synchronously; it must not re-enter helper. */
using MvHandPreferenceSink = void (*)(void *user, const MvHandPreferenceEvent *event);

struct MvHandPreferenceEntry {
    uint64_t container;
    uint32_t index;
    int32_t baseline;
    vr::PropertyTypeTag_t baseline_tag;
    vr::ETrackedPropertyError baseline_error;
    bool captured, attempted, applied, finished;
};
struct MvHandPreference {
    vr::IVRProperties *properties;
    MvHandPreferenceSink sink;
    void *user;
    bool enabled;
    MvHandPreferenceEntry hands[2];
};

extern "C" {
/* Caller-owned context, serialized on the experimental worker. No thread,
 * clock, phase, SDK initialization or writes here. Explicitly opt in. */
void mv_hand_preference_init(MvHandPreference *, vr::IVRProperties *, bool enabled,
                             MvHandPreferenceSink, void *user);
/* Caller invokes at most once/second. One write attempt per captured HAND
 * container; missing/invalid baselines remain unhandled. No role reversal. */
void mv_hand_preference_apply_pending(MvHandPreference *);
/* Stop disables further applies. Call with interfaces still valid, before
 * original Cleanup. Repeated successful restore is a no-op. */
void mv_hand_preference_restore(MvHandPreference *);
}
#endif
