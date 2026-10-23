#ifndef MIXEDVR_INPUT_PROBE_H
#define MIXEDVR_INPUT_PROBE_H
#include <stdint.h>

#define MV_MAX_COMPONENTS 64
#define MV_TRACKPAD_TOUCH "/input/trackpad/touch"

/* All timestamps are local callback receipt time, never hardware sample time. */
typedef struct {
    uint64_t container, component, updates, phase_baseline, last_update_ms;
    uint64_t identity_generation;
    uint32_t device_index;
    uint8_t known, touched, identity_bound;
} MvComponent;

typedef struct {
    volatile long lock;
    uint64_t phase, overflow;
    MvComponent components[MV_MAX_COMPONENTS];
} MvProbe;

typedef struct {
    uint64_t container, generation;
    uint32_t device_index, role;
    uint8_t real_index; /* class=Controller AND lighthouse AND knuckles */
} MvIdentity;

typedef struct {
    double velocity[3], angular_velocity[3];
    uint32_t tracking_result;
    uint8_t connected, valid;
} MvPose;

typedef struct {
    uint64_t phase, updates, updates_in_phase, last_update_ms;
    uint32_t device_index, role;
    uint8_t known, touched, observed_this_phase, pose_known, moving, index_candidate, stationary;
} MvSample;

#ifdef __cplusplus
extern "C" {
#endif
/* Reset only at initialization / after callbacks have quiesced, not hot-unload. */
void mv_reset(MvProbe *probe);
void mv_mark_phase(MvProbe *probe);
void mv_created(MvProbe *probe, uint64_t container, const char *path,
                uint64_t component, int32_t original_result);
void mv_updated(MvProbe *probe, uint64_t component, uint8_t value,
                int32_t original_result, uint64_t now_ms);
/* Read-only diagnostic and dry-run predicate; never requests a mode/priority. */
int mv_sample(MvProbe *probe, const MvIdentity *identity, const MvPose *pose,
              MvSample *sample);
#ifdef __cplusplus
}
#endif
#endif
