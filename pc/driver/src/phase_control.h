#ifndef MV_TRIAL_PHASE_CONTROL_H
#define MV_TRIAL_PHASE_CONTROL_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* One local observation label. This never changes a tracking mode. */
int mv_parse_phase(const char *data, size_t bytes, uint64_t *sequence, uint32_t *phase);
#ifdef __cplusplus
}
#endif
#endif
