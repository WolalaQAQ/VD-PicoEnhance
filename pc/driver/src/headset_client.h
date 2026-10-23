#ifndef MV_TRIAL_HEADSET_CLIENT_H
#define MV_TRIAL_HEADSET_CLIENT_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { MV_HEADSET_NONE, MV_HEADSET_INDEX, MV_HEADSET_HANDS,
       MV_HEADSET_RESTORE, MV_HEADSET_STATUS };
enum { MV_HEADSET_UNINITIALIZED, MV_HEADSET_IDLE, MV_HEADSET_CONNECTING,
       MV_HEADSET_ONLINE, MV_HEADSET_FAILED, MV_HEADSET_CLOSED };
enum { MV_HEADSET_REQUESTED = 1, MV_HEADSET_CONNECTED, MV_HEADSET_ACKNOWLEDGED,
       MV_HEADSET_DISCONNECTED, MV_HEADSET_RESTORE_UNCONFIRMED, MV_HEADSET_CLOSE_EVENT };
enum { MV_HEADSET_OK, MV_HEADSET_CONNECT_ERROR, MV_HEADSET_CONNECT_TIMEOUT,
       MV_HEADSET_SEND_ERROR, MV_HEADSET_RECEIVE_ERROR, MV_HEADSET_PEER_CLOSED,
       MV_HEADSET_BAD_FRAMING, MV_HEADSET_BAD_ACK, MV_HEADSET_MODE_MISMATCH,
       MV_HEADSET_HELPER_ERROR, MV_HEADSET_ACK_TIMEOUT, MV_HEADSET_PROCESS_PAUSE,
       MV_HEADSET_SEQUENCE_EXHAUSTED, MV_HEADSET_SHUTDOWN_TIMEOUT,
       MV_HEADSET_CLOCK_REVERSED, MV_HEADSET_FORCED_CLOSE };
enum { MV_HEADSET_SETTING_UNKNOWN = -1, MV_HEADSET_SETTING_OFF,
       MV_HEADSET_SETTING_ON, MV_HEADSET_SETTING_NULL, MV_HEADSET_SETTING_ERROR };
enum { MV_HEADSET_UNOBSERVED, MV_HEADSET_BLOCKED, MV_HEADSET_WARMING, MV_HEADSET_READY };
enum { MV_HEADSET_IO_ERROR = -1, MV_HEADSET_IO_WAIT = -2 };
enum { MV_HEADSET_LEASE_MS = 3000, MV_HEADSET_HEARTBEAT_MS = 500,
       MV_HEADSET_ACK_TIMEOUT_MS = 2000, MV_HEADSET_CONNECT_TIMEOUT_MS = 500 };

/* Fixed values only: no wire strings, token, session or peer-provided log text.
 * ACKNOWLEDGED means ConfigurationService readback, NOT SteamVR source selection. */
typedef struct {
    uint64_t sequence;
    uint32_t kind, command, reason;
    int32_t mode, setting;
    uint32_t vd_phase, helper_ok;
} MvHeadsetEvent;

typedef struct {
    void *user;
    /* open starts ONE loopback connection; poll only checks that same attempt.
     * Both return 1=connected, 0=pending, -1=failed. All calls are nonblocking.
     * close releases even a partially failed open, and is called once per open. */
    int (*open)(void *user, uint16_t loopback_port);
    int (*poll)(void *user);
    /* Positive=bytes; IO_WAIT=would block; IO_ERROR=failure; zero=closed.
     * send/recv may transfer any prefix up to size. They never log wire bytes. */
    int (*send)(void *user, const char *bytes, uint32_t size);
    int (*recv)(void *user, char *bytes, uint32_t capacity);
    void (*close)(void *user);
    /* random is only required when init's session32 is NULL. Return 1 on success. */
    int (*random)(void *user, unsigned char *bytes, uint32_t size);
    uint64_t (*clock_ms)(void *user);
    void (*event)(void *user, const MvHeadsetEvent *event);
} MvHeadsetBindings;

typedef struct {
    MvHeadsetBindings io;
    uint64_t sequence, pending_sequence, pending_at, next_send_at;
    uint64_t last_tick, connect_at, shutdown_at;
    uint32_t state, desired, pending_command, command_due, status_due;
    uint32_t opened, stopping, restore_confirmed;
    uint32_t outgoing_size, outgoing_offset, incoming_size;
    uint16_t port;
    char token[64], session[32], outgoing[160], incoming[128];
} MvHeadsetClient;

/* One owner worker; calls/callbacks are serial and non-reentrant. No allocation,
 * CRT, OS calls or implicit I/O. token64/session32 are exact lowercase hex byte
 * spans (no terminator needed). NULL session uses 16 injected random bytes.
 * Before re-init, shutdown/close the previous instance. Init is the ONLY explicit
 * enable: after ANY failure neither request nor tick reconnects. A caller-owned
 * discovery policy may start a fresh session after recovery; input/heartbeat
 * requests alone never restart this client. */
int mv_headset_init(MvHeadsetClient *client, const MvHeadsetBindings *bindings,
                    uint16_t port, const char *token64, const char *session32);
/* Latest requested mode is coalesced; one pending ACK at a time. Duplicate mode
 * requests do not reset deadlines or flood heartbeats. STATUS never revives a
 * failed lease. No connect/send occurs until tick. */
int mv_headset_request(MvHeadsetClient *client, uint32_t command);
void mv_headset_tick(MvHeadsetClient *client, uint64_t now_ms);
/* Cleanup: begin once, then tick while waiting on the owner worker (e.g. 10ms
 * waits, max 2000ms). Existing partial sends/ACKs finish before RESTORE. A final
 * close is allowed if cleanup cannot wait; helper disconnect/expiry restores.
 * shutdown_tick returns 1 when closed/failed, 0 while work remains. */
void mv_headset_shutdown_begin(MvHeadsetClient *client, uint64_t now_ms);
int mv_headset_shutdown_tick(MvHeadsetClient *client, uint64_t now_ms);
void mv_headset_close(MvHeadsetClient *client);

#ifdef __cplusplus
}
#endif
#endif
