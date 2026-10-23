// Owned deterministic fixture: real daemon control loop/core, fake OS and Binder.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#define API __declspec(dllexport)
#define PRIu64 "llu"
#define SCNu64 "llu"
static int (*common_vsprintf)(uint64_t, char *, size_t, const char *, void *, va_list);
static int (*common_vsscanf)(uint64_t, const char *, size_t, const char *, void *, va_list);
static int *(*runtime_errno)(void);
static int runtime_snprintf(char *out, size_t cap, const char *format, ...) {
    va_list args; va_start(args, format);
    int n = common_vsprintf(2, out, cap, format, NULL, args);
    va_end(args); return n;
}
static int runtime_sscanf(const char *input, const char *format, ...) {
    va_list args; va_start(args, format);
    int n = common_vsscanf(0, input, (size_t)-1, format, NULL, args);
    va_end(args); return n;
}
static int (*runtime_strcmp)(const char *, const char *);
static size_t (*runtime_strlen)(const char *);
static void *(*runtime_memcpy)(void *, const void *, size_t);
static void *(*runtime_memmove)(void *, const void *, size_t);
static void *(*runtime_memchr)(const void *, int, size_t);
static unsigned long long (*runtime_strtoull)(const char *, char **, int);
static int (*runtime_atoi)(const char *);
#define snprintf runtime_snprintf
#define sscanf runtime_sscanf
#define strcmp runtime_strcmp
#define strlen runtime_strlen
#define memcpy runtime_memcpy
#define memmove runtime_memmove
#define memchr runtime_memchr
#define strtoull runtime_strtoull
#define atoi runtime_atoi
void *memset(void *p, int value, size_t length) {
    unsigned char *bytes = p;
    for (size_t i = 0; i < length; ++i) bytes[i] = (unsigned char)value;
    return p;
}
API void set_crt(void **p) {
    common_vsprintf = p[0]; common_vsscanf = p[1]; runtime_strcmp = p[2];
    runtime_strlen = p[3]; runtime_memcpy = p[4]; runtime_memmove = p[5];
    runtime_memchr = p[6]; runtime_strtoull = p[7]; runtime_atoi = p[8]; runtime_errno = p[9];
}

typedef unsigned uid_t;
typedef unsigned gid_t;
typedef long long ssize_t;
typedef unsigned socklen_t;
struct stat { unsigned st_mode, st_uid; };
#define S_ISDIR(m) (((m) & 0170000) == 0040000)
#define S_ISREG(m) (((m) & 0170000) == 0100000)
#define O_RDONLY 1
#define O_CLOEXEC 2
#define O_DIRECTORY 4
#define O_NOFOLLOW 8
#define LOCK_EX 1
#define LOCK_NB 2
#define F_OK 0
#define ENOENT 2
#define EINTR 4
#define EAGAIN 11
#define EPERM 1
#define EIO 5
#define EMFILE 24
#define EADDRINUSE 98
#define EADDRNOTAVAIL 99
#define errno (*runtime_errno())
#define ROOT "/data/adb/pico_mixedvr_standalone"
#define PORT 29744
#define MAX_LEASE 5000
#define AF_INET 2
#define SOCK_STREAM 1
#define SOCK_CLOEXEC 2
#define SOCK_NONBLOCK 4
#define SOCK_DGRAM 8
#define SOL_SOCKET 1
#define SO_REUSEADDR 2
#define IPPROTO_IP 0
#define IP_FREEBIND 15
#define IP_ADD_MEMBERSHIP 35
#define SIOCGIFADDR 1
#define SIOCGIFNETMASK 2
#define POLLIN 1
#define POLLHUP 2
#define POLLERR 4
#define POLLNVAL 8
#define MSG_NOSIGNAL 1
struct sockaddr { int unused; };
struct sockaddr_in { unsigned sin_family, sin_port; struct { uint32_t s_addr; } sin_addr; };
struct ip_mreq { struct { uint32_t s_addr; } imr_multiaddr, imr_interface; };
struct ifreq { char ifr_name[16]; struct sockaddr_in ifr_addr; };
#define ifr_netmask ifr_addr
struct pollfd { int fd, events, revents; };
static uint32_t htonl(uint32_t n) { return (n >> 24) | ((n >> 8) & 0xff00) | ((n << 8) & 0xff0000) | (n << 24); }
#define ntohl htonl
static unsigned htons(unsigned n) { return ((n & 255) << 8) | (n >> 8); }
#include "lan_config.h"

static uint64_t clock_ms, deadline, operation_deadline;
static int mode, baseline_consumer, stopping;
static bool write_pending, restoring_system, owned, restore_pending;
static char baseline[8], expected[8], previous_value[8], token[65];
static int db, consumer, snapshot_value, journal_present, enabled, network_valid;
static int unsafe, snapshots, sets, renotifies, starts, stops, restores, accepts, reads;
static int fail_mode, fail_restore, metadata_ok, inflight_ok;
static int legacy_exists, legacy_requested, legacy_pending, legacy_locked, lock_fd;
static int address_assigned, freebind_set, fail_freebind, port_busy, fail_socket, fail_listen, fail_ready;
static int ready_writes, bind_calls, freebind_calls, listen_calls, server_closes;
static uint32_t current_address, current_mask;
static int next_udp, udp_replies, fail_discovery;
static char discovery_reply[LAN_DISCOVERY_SIZE + 1];
static uint64_t now_ms(void) { return clock_ms; }
static bool settle_inflight(void) { return inflight_ok != 0; }
static bool atomic_file(const char *path, const char *text, uid_t uid, gid_t gid) {
    (void)text;
    if (uid || gid) ++unsafe;
    if (!strcmp(path, ROOT "/ready")) {
        if (fail_ready) { errno = EIO; return false; }
        ++ready_writes;
    }
    if (!strcmp(path, ROOT "/baseline")) journal_present = 1;
    else if (strcmp(path, ROOT "/lease") && strcmp(path, ROOT "/ready")) ++unsafe;
    return true;
}
static bool setting_get(char out[8]) { snprintf(out, 8, "%d", db); return true; }
static bool configuration_value(const char *operation, char out[8]) {
    int value = db;
    if (!strcmp(operation, "snapshot")) { ++snapshots; snapshot_value = db; }
    else if (!strcmp(operation, "baseline")) value = snapshot_value;
    else if (!strcmp(operation, "verify") && !metadata_ok) return false;
    snprintf(out, 8, "%d", value); return true;
}
static bool setting_set(const char *wanted, const char *previous) {
    if (atoi(previous) != db || !metadata_ok) return false;
    ++sets;
    if (restoring_system) {
        ++restores;
        if (fail_restore) return false;
    }
    if (fail_mode && !restoring_system) { consumer = atoi(wanted); return false; }
    int previous_db = db;
    db = atoi(wanted);
    // Same-value ordinary setter may short-circuit its notification.
    if (previous_db != db) consumer = db;
    return true;
}
static bool configuration_call(const char *operation, const char *previous, char *out, size_t cap) {
    (void)previous; (void)out; (void)cap;
    if (strcmp(operation, "renotify-baseline") || db != snapshot_value || !metadata_ok) { ++unsafe; return false; }
    ++renotifies; consumer = snapshot_value; return true;
}
static bool tracking_config(const char *wanted) { return atoi(wanted) == consumer; }
static bool tracking_call(const char *operation, char *out, size_t cap) {
    if (!strcmp(operation, "query")) snprintf(out, cap, "tracking_config=%d", consumer);
    else if (!strcmp(operation, "start")) ++starts;
    else if (!strcmp(operation, "stop")) ++stops;
    else ++unsafe;
    return true;
}
static int open(const char *path, int flags) {
    if (!strcmp(path, ROOT)) { if (!(flags & O_DIRECTORY)) ++unsafe; return 10; }
    if (!strcmp(path, "/data/adb/vdhs_mixedvr/supervisor.lock")) {
        if (flags != (O_RDONLY | O_CLOEXEC | O_NOFOLLOW)) ++unsafe;
        return lock_fd;
    }
    ++unsafe; return -1;
}
static int fstat(int fd, struct stat *st) { (void)fd; st->st_mode = 0100000; st->st_uid = 0; return 0; }
static int flock(int fd, int flags) { if (fd != lock_fd || flags != (LOCK_EX | LOCK_NB)) ++unsafe; return legacy_locked ? -1 : 0; }
static int fsync(int fd) { if (fd != 10) ++unsafe; return 0; }
static int close(int fd) { if (fd == 90) ++server_closes; return 0; }
static int unlink(const char *path) {
    if (!strcmp(path, ROOT "/baseline")) journal_present = 0;
    else if (strcmp(path, ROOT "/ready")) ++unsafe;
    return 0;
}
static int lstat(const char *path, struct stat *st) {
    if (!strcmp(path, "/data/adb/vdhs_mixedvr")) {
        if (!legacy_exists) { errno = ENOENT; return -1; }
        st->st_mode = 0040000; st->st_uid = 0; return 0;
    }
    const char *paths[] = {"/data/adb/vdhs_mixedvr/enabled", "/data/adb/vdhs_mixedvr/baseline", "/data/adb/vdhs_mixedvr/inflight"};
    for (int i = 0; i < 3; ++i) if (!strcmp(paths[i], path)) {
        if (legacy_pending & (1 << i)) { st->st_mode = 0100000; st->st_uid = 0; return 0; }
        errno = ENOENT; return -1;
    }
    ++unsafe; return -1;
}
static bool read_small(const char *path, char *out, size_t cap, bool root_only) {
    if (!root_only) ++unsafe;
    if (!strcmp(path, ROOT "/network")) {
        if (!network_valid) return false;
        snprintf(out, cap, "%u.%u.%u.%u %u.%u.%u.%u", 10u, 0u, 0u, 2u, 10u, 0u, 0u, 3u); return true;
    }
    if (!strcmp(path, "/data/adb/vdhs_mixedvr/requested")) {
        if (legacy_requested < 0) return false;
        snprintf(out, cap, "%d", legacy_requested); return true;
    }
    ++unsafe; return false;
}
#include "migration_guard.h"
#include "core_source.h"

struct event { uint64_t time; int action; char text[256]; };
static struct event events[32], *current;
static int event_count, event_index;
static char replies[4096], diagnostics[2048];
static uint32_t bound_address;
static int bound_port;
static ssize_t send(int fd, const char *text, size_t length, int flags) {
    if (fd != 99 || flags != MSG_NOSIGNAL || strlen(replies) + length >= sizeof replies) { ++unsafe; return -1; }
    memcpy(replies + strlen(replies), text, length + 1); return (ssize_t)length;
}
#include "reply_source.h"
static int socket(int domain, int type, int protocol) {
    if (domain != AF_INET || protocol) ++unsafe;
    if (type == (SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK)) {
        if (fail_discovery) return -1;
        return next_udp++;
    }
    if (type != (SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK)) ++unsafe;
    if (fail_socket) { errno = EMFILE; return -1; }
    return 90;
}
static int setsockopt(int fd, int level, int name, const void *value, size_t size) {
    if (fd >= 92) {
        if (level == SOL_SOCKET && name == SO_REUSEADDR && size == sizeof(int) && *(const int *)value == 1) return 0;
        if (level == IPPROTO_IP && name == IP_ADD_MEMBERSHIP && size == sizeof(struct ip_mreq)) {
            const struct ip_mreq *group = value;
            if (ntohl(group->imr_multiaddr.s_addr) != 0xeffffffa || ntohl(group->imr_interface.s_addr) != current_address) ++unsafe;
            return 0;
        }
        ++unsafe; return -1;
    }
    if (fd != 90 || size != sizeof(int) || *(const int *)value != 1) { ++unsafe; return -1; }
    if (level == SOL_SOCKET && name == SO_REUSEADDR) return 0;
    if (level != IPPROTO_IP || name != IP_FREEBIND) { ++unsafe; return -1; }
    ++freebind_calls;
    if (fail_freebind) { errno = EPERM; return -1; }
    freebind_set = 1;
    return 0;
}
static int bind(int fd, const struct sockaddr *address, size_t size) {
    const struct sockaddr_in *in = (const struct sockaddr_in *)address;
    if (fd >= 92) {
        uint32_t ip = ntohl(in->sin_addr.s_addr);
        if (size != sizeof *in || (ip != current_address && ip != 0xeffffffa) || htons(in->sin_port) != PORT) ++unsafe;
        return 0;
    }
    if (fd != 90 || size != sizeof *in) ++unsafe;
    bound_address = ntohl(in->sin_addr.s_addr); bound_port = (int)htons(in->sin_port); ++bind_calls;
    if (port_busy) { errno = EADDRINUSE; return -1; }
    if (!address_assigned && !freebind_set) { errno = EADDRNOTAVAIL; return -1; }
    return 0;
}
static int listen(int fd, int backlog) {
    if (fd != 90 || backlog != 1) ++unsafe;
    ++listen_calls;
    if (fail_listen) { errno = EIO; return -1; }
    return 0;
}
static int ioctl(int fd, int operation, struct ifreq *request) {
    if (strcmp(request->ifr_name, "wlan0")) ++unsafe;
    if (fd < 0 || !address_assigned || !current_address) return -1;
    if (operation != SIOCGIFADDR && operation != SIOCGIFNETMASK) ++unsafe;
    request->ifr_addr.sin_addr.s_addr = htonl(operation == SIOCGIFADDR ? current_address : current_mask);
    return 0;
}
static int getpid(void) { return 43; }
static int getppid(void) { return 42; }
static int access(const char *path, int flags) { if (strcmp(path, ROOT "/enabled") || flags) ++unsafe; return enabled ? 0 : -1; }
static int accept4(int fd, struct sockaddr *address, socklen_t *size, int flags) {
    (void)flags;
    struct sockaddr_in *in = (struct sockaddr_in *)address;
    if (fd != 90 || *size != sizeof *in) ++unsafe;
    in->sin_family = AF_INET;
    in->sin_addr.s_addr = htonl(current->action == 7 || current->action == 12 ? 0x0a000004 : 0x0a000003);
    ++accepts; return 99;
}
static ssize_t recv(int fd, void *out, size_t cap, int flags) {
    (void)flags; ++reads;
    if (fd != 99) ++unsafe;
    if (current->action == 3) return 0;
    size_t n = strlen(current->text);
    if (n > cap) n = cap;
    memcpy(out, current->text, n); return (ssize_t)n;
}
static ssize_t recvfrom(int fd, void *out, size_t cap, int flags, struct sockaddr *address, socklen_t *size) {
    if (fd < 92 || flags || *size != sizeof(struct sockaddr_in)) ++unsafe;
    struct sockaddr_in *peer = (struct sockaddr_in *)address;
    peer->sin_family = AF_INET; peer->sin_port = htons(40000);
    peer->sin_addr.s_addr = htonl(current->action == 13 ? 0x0a010004 : current->action == 9 ? 0x0a000003 : 0x0a000004);
    size_t n = strlen(current->text);
    if (n > cap) n = cap;
    memcpy(out, current->text, n); return (ssize_t)n;
}
static ssize_t sendto(int fd, const void *data, size_t length, int flags, const struct sockaddr *address, socklen_t size) {
    (void)address;
    if (fd < 92 || length != LAN_DISCOVERY_SIZE || flags != MSG_NOSIGNAL || size != sizeof(struct sockaddr_in)) ++unsafe;
    memcpy(discovery_reply, data, length); discovery_reply[length] = 0; ++udp_replies;
    return (ssize_t)length;
}
static int poll(struct pollfd *pf, int count, int timeout) {
    if (count != 4 || timeout != 100) ++unsafe;
    for (int i = 0; i < count; ++i) pf[i].revents = 0;
    if (event_index == event_count) { stopping = 1; return 0; }
    current = &events[event_index++]; clock_ms = current->time;
    switch (current->action) {
    case 1: case 7: case 12: pf[0].revents = POLLIN; break;
    case 2: case 3: pf[1].revents = POLLIN; break;
    case 6: enabled = 0; break;
    case 8: pf[1].revents = POLLHUP; break;
    case 9: pf[2].revents = POLLIN; break;
    case 11: case 13: pf[3].revents = POLLIN; break;
    case 10: current_address = 0x0a000007; break;
    case 14: current_address = 0x7f000001; break;
    case 16: fail_restore = 1; break;
    case 17: fail_restore = 0; break;
    case 18: current_address = 0; break;
    case 20: current_address = 0x0a000007; port_busy = 1; break;
    case 21: port_busy = 0; break;
    case 22: current_address = 0x0a000009; port_busy = 0; break;
    }
    return 1;
}
static int fake_fprintf(void *stream, const char *format, ...) {
    (void)stream;
    size_t used = strlen(diagnostics);
    va_list args; va_start(args, format);
    int n = common_vsprintf(2, diagnostics + used, sizeof diagnostics - used, format, NULL, args);
    va_end(args);
    if (n < 0 || (size_t)n >= sizeof diagnostics - used) ++unsafe;
    return n;
}
#define fprintf fake_fprintf
#define stderr ((void *)0)
#include "serve_source.h"

API void reset(int initial) {
    clock_ms = 10000; deadline = operation_deadline = 0;
    mode = baseline_consumer = errno = stopping = 0;
    write_pending = restoring_system = owned = restore_pending = false;
    memset(baseline, 0, sizeof baseline); memset(expected, 0, sizeof expected);
    memset(previous_value, 0, sizeof previous_value); memset(token, 'a', 64); token[64] = 0;
    db = consumer = initial; snapshot_value = -1; journal_present = 0;
    enabled = network_valid = metadata_ok = inflight_ok = 1;
    unsafe = snapshots = sets = renotifies = starts = stops = restores = accepts = reads = 0;
    fail_mode = fail_restore = 0;
    legacy_exists = 1; legacy_requested = legacy_pending = legacy_locked = 0; lock_fd = 70;
    event_count = event_index = 0; replies[0] = 0; bound_address = 0; bound_port = 0;
    diagnostics[0] = 0; address_assigned = 1;
    freebind_set = fail_freebind = port_busy = fail_socket = fail_listen = fail_ready = 0;
    ready_writes = bind_calls = freebind_calls = listen_calls = server_closes = 0;
    current_address = 0x0a000002; current_mask = 0xffffff00;
    next_udp = 92; udp_replies = fail_discovery = 0; discovery_reply[0] = 0;
}
API int parse_network(const char *text) { struct lan_config config; return lan_parse(text, &config); }
API void add(uint64_t time, int action, const char *text) {
    struct event *e = &events[event_count++]; e->time = time; e->action = action;
    snprintf(e->text, sizeof e->text, "%s", text);
}
API int run(void) { return serve(); }
API const char *answers(void) { return replies; }
API const char *startup_log(void) { return diagnostics; }
API const char *discovery_answer(void) { return discovery_reply; }
API int bind_without_freebind(void) {
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(PORT) };
    addr.sin_addr.s_addr = htonl(0x0a000002);
    return bind(90, (const struct sockaddr *)&addr, sizeof addr);
}
API int control(int wanted, uint64_t lease) { return apply(wanted, lease); }
API int recover(void) { return restore(); }
API void option(int which, int value) {
    switch (which) {
    case 0: fail_mode = value; break; case 1: fail_restore = value; break;
    case 2: metadata_ok = value; break; case 3: inflight_ok = value; break;
    case 4: network_valid = value; break; case 5: db = value; break;
    case 6: consumer = value; break; case 7: legacy_exists = value; break;
    case 8: legacy_requested = value; break; case 9: legacy_pending = value; break;
    case 10: legacy_locked = value; break;
    case 11: address_assigned = value; break; case 12: fail_freebind = value; break;
    case 13: port_busy = value; break; case 14: fail_socket = value; break;
    case 15: fail_listen = value; break; case 16: fail_ready = value; break;
    case 17: fail_discovery = value; break;
    }
}
API uint64_t value(int which) {
    switch (which) {
    case 0: return mode; case 1: return db; case 2: return consumer;
    case 3: return journal_present; case 4: return owned; case 5: return deadline;
    case 6: return snapshots; case 7: return sets; case 8: return renotifies;
    case 9: return starts; case 10: return stops; case 11: return restores;
    case 12: return accepts; case 13: return reads; case 14: return unsafe;
    case 15: return bound_address; case 16: return bound_port; case 17: return restore_pending;
    case 18: return freebind_set; case 19: return ready_writes; case 20: return bind_calls;
    case 21: return freebind_calls; case 22: return listen_calls; case 23: return server_closes;
    case 24: return (unsigned)errno;
    case 25: return udp_replies; case 26: return (unsigned)next_udp - 92;
    } return 0;
}
API int migration(void) { int held = -1; return migration_guard(&held) ? held + 2 : 0; }
