#ifndef MV_TRIAL_HEADSET_TRANSPORT_H
#define MV_TRIAL_HEADSET_TRANSPORT_H
#include "headset_client.h"
#include "native_api.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void *(*MvHeadsetGetProc)(void *module, const char *name);
typedef void (*MvHeadsetEventSink)(void *user, const MvHeadsetEvent *event);

/* Win64 ABI only; opaque sockaddr/fd_set/WSADATA arguments are private to .c.
 * Function pointers are resolved from explicit System32 library loads. */
typedef struct {
    void *(*load_library_ex)(const wchar_t *, void *, uint32_t);
    int32_t (*free_library)(void *);
    int32_t (*startup)(uint16_t, void *);
    int32_t (*cleanup)(void);
    uintptr_t (*socket)(int32_t, int32_t, int32_t);
    int32_t (*nonblocking)(uintptr_t, uint32_t, uint32_t *);
    int32_t (*connect)(uintptr_t, const void *, int32_t);
    int32_t (*select)(int32_t, void *, void *, void *, const void *);
    int32_t (*get_option)(uintptr_t, int32_t, int32_t, char *, int32_t *);
    int32_t (*send)(uintptr_t, const char *, int32_t, int32_t);
    int32_t (*recv)(uintptr_t, char *, int32_t, int32_t);
    int32_t (*close_socket)(uintptr_t);
    int32_t (*last_error)(void);
    int32_t (*random)(void *, unsigned char *, uint32_t, uint32_t);
} MvHeadsetSocketApi;

typedef struct {
    MvHeadsetSocketApi api;
    const MvNativeApi *native;
    MvHeadsetEventSink sink;
    void *sink_user, *winsock_module, *bcrypt_module;
    uintptr_t socket;
    uint32_t started, has_socket;
} MvHeadsetTransport;

/* Called ONLY for a new explicit auto marker, never from automatic retries.
 * Reads token_path (64 lowercase hex, optional LF/CRLF), creates a new session
 * with System32 BCryptGenRandom, and initializes client for 127.0.0.1:29743.
 * Missing/invalid file fails closed. Does not open any socket until client tick.
 * get_proc is GetProcAddress supplied separately by the caller (e.g. original
 * resolved IAT); native_api is unchanged, and no original DLL base is needed.
 * native and transport outlive client. Before re-init: finish/close client, then
 * transport_cleanup. A failed init already cleans its own partial resources. */
int mv_headset_transport_init(MvHeadsetTransport *transport, MvHeadsetClient *client,
                             const MvNativeApi *native, MvHeadsetGetProc get_proc,
                             const wchar_t *token_path, MvHeadsetEventSink sink, void *sink_user);
/* Only after client shutdown/close; never while it can tick. Idempotent. */
void mv_headset_transport_cleanup(MvHeadsetTransport *transport);

#ifdef __cplusplus
}
#endif
#endif
