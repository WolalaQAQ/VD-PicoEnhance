#ifndef MV_TRIAL_NATIVE_API_H
#define MV_TRIAL_NATIVE_API_H
#include <stdint.h>
#include <stddef.h>

typedef uint32_t (*MvThreadMain)(void *);
typedef struct {
    void *(*create_thread)(void *, size_t, MvThreadMain, void *, uint32_t, uint32_t *);
    void *(*create_event)(void *, const wchar_t *, uint32_t, uint32_t);
    int32_t (*set_event)(void *);
    uint32_t (*wait)(void *, uint32_t);
    int32_t (*close)(void *);
    uint64_t (*clock_ms)(void);
    void *(*module)(const wchar_t *);
    void *(*open_file)(const wchar_t *, uint32_t, uint32_t, void *, uint32_t, uint32_t, void *);
    int32_t (*read_file)(void *, void *, uint32_t, uint32_t *, void *);
    uint32_t (*last_error)(void);
    uint32_t (*process_id)(void);
} MvNativeApi;

#ifdef __cplusplus
extern "C" {
#endif
void mv_native_bind(MvNativeApi *api, uintptr_t base);
#ifdef __cplusplus
}
#endif
#endif
