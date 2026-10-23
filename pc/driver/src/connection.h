#ifndef VD_PICO_CONNECTION_H
#define VD_PICO_CONNECTION_H
#include "headset_transport.h"

typedef struct {
    wchar_t config[1536], command[1536];
    const wchar_t *adb, *serial;
    const MvNativeApi *native;
    int32_t (*create_process)(const wchar_t *, wchar_t *, void *, void *, int32_t,
                              uint32_t, void *, const wchar_t *, void *, void *);
    int32_t (*terminate_process)(void *, uint32_t);
    void *process;
    uint64_t deadline;
    uint32_t stage;
} MvConnection;

#ifdef __cplusplus
extern "C" {
#endif
/* Worker-only bootstrap. Missing config retains an existing legacy forward.
 * Returns 1 ready, 0 pending, -1 failed. No shell or runtime script is launched. */
int mv_connection_start(MvConnection *, const MvNativeApi *, MvHeadsetGetProc,
                        const wchar_t *path, uint64_t now);
int mv_connection_tick(MvConnection *, uint64_t now);
void mv_connection_stop(MvConnection *);
#ifdef __cplusplus
}
#endif
#endif
