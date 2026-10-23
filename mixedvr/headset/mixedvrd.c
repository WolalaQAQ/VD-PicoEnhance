// Root companion. Fixed commands only; app-private text/loopback, bounded
// leases and a durable ConfigurationService baseline. No system_server injection.
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/prctl.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define ROOT "/data/adb/vdhs_mixedvr"
#define PORT 29743
#define MAX_LEASE 5000

static volatile sig_atomic_t stopping;
static uint64_t deadline;
static int mode; // 0=baseline/restore, 1=index, 2=hands
static char baseline[8];
static char expected[8];
static char previous_value[8];
static bool write_pending;
static int baseline_consumer;
static bool restoring_system;
static bool owned;
static bool restore_pending;
static uint64_t operation_deadline;
static char token[65];
static char tracking_jar[512];
static pid_t backend_pid = -1;
static int backend_in = -1, backend_out = -1;
static bool backend_shared;
static uint64_t backend_token, rpc_counter;
static char backend_buffer[512];
static size_t backend_used;

static uint64_t now_ms(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_BOOTTIME, &t)) return 0;
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}

static void on_signal(int sig) { (void)sig; stopping = 1; }

static bool write_all(int fd, const char *p, size_t n) {
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return false;
        p += w; n -= (size_t)w;
    }
    return true;
}

static bool atomic_file(const char *path, const char *text, uid_t uid, gid_t gid) {
    char tmp[512];
    if (snprintf(tmp, sizeof tmp, "%s.XXXXXX", path) >= (int)sizeof tmp) return false;
    int fd = mkstemp(tmp);
    if (fd < 0) return false;
    bool ok = fchmod(fd, 0600) == 0 && fchown(fd, uid, gid) == 0 &&
              write_all(fd, text, strlen(text)) && fsync(fd) == 0;
    if (close(fd)) ok = false;
    if (ok && rename(tmp, path)) ok = false;
    if (!ok) unlink(tmp);
    // Ensure the rename (not just its inode) survives a helper crash/reboot.
    char parent[512];
    snprintf(parent, sizeof parent, "%s", path);
    char *slash = strrchr(parent, '/');
    if (slash) {
        *slash = 0;
        int dir = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (dir < 0 || fsync(dir)) ok = false;
        if (dir >= 0) close(dir);
    }
    return ok;
}

static bool read_small(const char *path, char *out, size_t cap, bool root_only) {
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return false;
    struct stat st;
    bool ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) &&
              st.st_size > 0 && st.st_size < (off_t)cap &&
              (!root_only || (st.st_uid == 0 && (st.st_mode & 0077) == 0));
    ssize_t n = ok ? read(fd, out, cap - 1) : -1;
    close(fd);
    if (n <= 0 || n != st.st_size) return false;
    out[n] = 0;
    while (n && (out[n - 1] == '\n' || out[n - 1] == '\r')) out[--n] = 0;
    return true;
}

// The pipe accepts fixed configuration/tracking operations only, never a shell.
static void backend_stop(void) {
    if (backend_in >= 0) close(backend_in);
    if (backend_out >= 0) close(backend_out);
    backend_in = backend_out = -1;
    if (backend_pid > 0 && !backend_shared) {
        kill(backend_pid, SIGKILL);
        while (waitpid(backend_pid, NULL, 0) < 0 && errno == EINTR) {}
    }
    backend_pid = -1;
    backend_used = 0;
}

static bool backend_start(void) {
    if (backend_pid > 0) return true;
    if (backend_shared) return false; // Never replace the supervisor's bridge.
    if (!tracking_jar[0] || access(tracking_jar, R_OK)) return false;
    int to_child[2], from_child[2];
    if (pipe2(to_child, O_CLOEXEC)) return false;
    if (pipe2(from_child, O_CLOEXEC)) { close(to_child[0]); close(to_child[1]); return false; }
    pid_t parent = getpid();
    pid_t pid = fork();
    if (pid < 0) { close(to_child[0]); close(to_child[1]); close(from_child[0]); close(from_child[1]); return false; }
    if (!pid) {
        if (prctl(PR_SET_PDEATHSIG, SIGKILL) || getppid() != parent) _exit(126);
        dup2(to_child[0], STDIN_FILENO); dup2(from_child[1], STDOUT_FILENO);
        close(to_child[0]); close(to_child[1]); close(from_child[0]); close(from_child[1]);
        int nullfd = open("/dev/null", O_WRONLY);
        if (nullfd >= 0) { dup2(nullfd, STDERR_FILENO); close(nullfd); }
        setenv("CLASSPATH", tracking_jar, 1);
        char *args[] = {"/system/bin/app_process", "/system/bin", "org.vdpico.mixedvr.ControlSession", NULL};
        execv(args[0], args); _exit(127);
    }
    close(to_child[0]); close(from_child[1]);
    backend_pid = pid; backend_in = to_child[1]; backend_out = from_child[0];
    if (getrandom(&backend_token, sizeof backend_token, 0) != sizeof backend_token || !backend_token) { backend_stop(); return false; }
    if (fcntl(backend_in, F_SETFL, O_NONBLOCK) || fcntl(backend_out, F_SETFL, O_NONBLOCK)) { backend_stop(); return false; }
    return true;
}

static bool boot_id(char out[40]) {
    int fd = open("/proc/sys/kernel/random/boot_id", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    ssize_t n = read(fd, out, 39); close(fd);
    if (n < 36) return false;
    out[36] = 0;
    for (int i = 0; i < 36; ++i) if (!((out[i] >= '0' && out[i] <= '9') || (out[i] >= 'a' && out[i] <= 'f') || out[i] == '-')) return false;
    return true;
}

static bool mark_inflight(void) {
    char boot[40], record[96];
    if (!boot_id(boot)) return false;
    snprintf(record, sizeof record, "VBI1 %s %" PRIu64 "\n", boot, backend_token);
    return atomic_file(ROOT "/inflight", record, 0, 0);
}

static bool erase_inflight(void) {
    if (unlink(ROOT "/inflight") && errno != ENOENT) return false;
    int fd = open(ROOT, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    bool ok = fd >= 0 && fsync(fd) == 0;
    if (fd >= 0) close(fd);
    return ok;
}

static bool backend_rpc(const char *subsystem, const char *operation, const char *previous, char *out, size_t cap) {
    if (operation_deadline && now_ms() >= operation_deadline) return false;
    if (!backend_start()) return false;
    uint64_t id = ((uint64_t)(uint32_t)getpid() << 32) | (uint32_t)++rpc_counter;
    char command[128];
    int length = operation ? snprintf(command, sizeof command, "%" PRIu64 " %s %s%s%s\n", id, subsystem, operation, previous ? " " : "", previous ? previous : "") :
                             snprintf(command, sizeof command, "%" PRIu64 " B\n", id);
    if (length <= 0 || length >= (int)sizeof command) return false;
    bool mutating = operation && !strcmp(subsystem, "C") && (!strncmp(operation, "set-", 4) || !strcmp(operation, "renotify-baseline"));
    if (mutating && !mark_inflight()) return false;
    uint64_t until = now_ms() + 3000;
    if (operation_deadline && operation_deadline < until) until = operation_deadline;
    size_t sent = 0;
    while (now_ms() < until) {
        if (sent < (size_t)length) {
            ssize_t n = write(backend_in, command + sent, (size_t)length - sent);
            if (n > 0) sent += (size_t)n;
            else if (n < 0 && errno != EAGAIN && errno != EINTR) break;
        }
        ssize_t n = read(backend_out, backend_buffer + backend_used, sizeof backend_buffer - 1 - backend_used);
        if (n > 0) {
            backend_used += (size_t)n; backend_buffer[backend_used] = 0;
            char *newline;
            while ((newline = memchr(backend_buffer, '\n', backend_used))) {
                size_t line_length = (size_t)(newline - backend_buffer);
                char answer[512]; memcpy(answer, backend_buffer, line_length); answer[line_length] = 0;
                memmove(backend_buffer, newline + 1, backend_used - line_length - 1); backend_used -= line_length + 1;
                uint64_t received = 0; int offset = 0;
                if (sscanf(answer, "%" SCNu64 " %n", &received, &offset) != 1 || received != id) continue; // Drain an earlier timed-out reply.
                if (sent != (size_t)length) return false;
                char *payload = answer + offset;
                if (strncmp(payload, "OK ", 3)) return false;
                if (out && (strlen(payload + 3) >= cap)) return false;
                if (out) memcpy(out, payload + 3, strlen(payload + 3) + 1);
                if (mutating && !erase_inflight()) return false; // Completed synchronous setter, not merely a value readback.
                return !operation_deadline || now_ms() < operation_deadline;
            }
            if (backend_used == sizeof backend_buffer - 1) break;
        } else if (n == 0 || (errno != EAGAIN && errno != EINTR)) break;
        struct pollfd pollfd[2] = {{backend_out, POLLIN, 0}, {backend_in, sent < (size_t)length ? POLLOUT : 0, 0}};
        poll(pollfd, 2, 20);
    }
    // Do not kill/restart a bridge with an outstanding service-side write. The
    // supervisor retains this pipe; a barrier serializes recovery behind it.
    return false;
}

static bool settle_inflight(void) {
    if (access(ROOT "/inflight", F_OK)) return errno == ENOENT;
    char record[96], saved_boot[40], current_boot[40]; uint64_t saved_token = 0; int end = 0;
    if (!read_small(ROOT "/inflight", record, sizeof record, true) ||
        sscanf(record, "VBI1 %39s %" SCNu64 " %n", saved_boot, &saved_token, &end) != 2 || record[end] || !boot_id(current_boot)) return false;
    if (strcmp(saved_boot, current_boot)) return erase_inflight(); // Old kernel/services are gone.
    if (saved_token != backend_token || backend_pid <= 0) return false; // Completion unknown in this boot.
    char answer[32];
    return backend_rpc("B", NULL, NULL, answer, sizeof answer) && !strcmp(answer, "barrier") && erase_inflight();
}

static void export_backend(void) {
    char value[32];
    snprintf(value, sizeof value, "%d", backend_in); setenv("VDHS_PIPE_IN", value, 1);
    snprintf(value, sizeof value, "%d", backend_out); setenv("VDHS_PIPE_OUT", value, 1);
    snprintf(value, sizeof value, "%d", backend_pid); setenv("VDHS_PIPE_PID", value, 1);
    snprintf(value, sizeof value, "%" PRIu64, backend_token); setenv("VDHS_PIPE_TOKEN", value, 1);
    fcntl(backend_in, F_SETFD, 0); fcntl(backend_out, F_SETFD, 0);
}

static bool configuration_call(const char *operation, const char *previous, char *out, size_t cap) {
    return backend_rpc("C", operation, previous, out, cap);
}

static bool configuration_value(const char *operation, char out[8]) {
    char text[64];
    if (!configuration_call(operation, NULL, text, sizeof text)) return false;
    if (!strcmp(text, "configuration=0")) { memcpy(out, "0", 2); return true; }
    if (!strcmp(text, "configuration=1")) { memcpy(out, "1", 2); return true; }
    return false;
}

static bool setting_get(char out[8]) { return configuration_value("query", out); }

static bool setting_set(const char *value, const char *previous) {
    char text[64];
    if (!strcmp(value, "0")) return configuration_call("set-0", previous, text, sizeof text) && !strcmp(text, "configuration=0");
    if (!strcmp(value, "1")) return configuration_call("set-1", previous, text, sizeof text) && !strcmp(text, "configuration=1");
    return false;
}

static bool tracking_call(const char *operation, char *out, size_t cap) {
    return backend_rpc("T", operation, NULL, out, cap);
}

static bool tracking_config(const char *expected_value) {
    char reply[64];
    uint64_t until = now_ms() + 750;
    do {
        if (!tracking_call("query", reply, sizeof reply)) return false;
        if ((!strcmp(expected_value, "0") && !strcmp(reply, "tracking_config=0")) ||
            (!strcmp(expected_value, "1") && !strcmp(reply, "tracking_config=1"))) return true;
        struct timespec delay = {0, 50000000}; nanosleep(&delay, NULL);
    } while (now_ms() < until && (!operation_deadline || now_ms() < operation_deadline));
    return false;
}

static bool journal(void) {
    char text[96];
    snprintf(text, sizeof text, "VDB3 %s %s %s %d %d %c\n", baseline, expected, previous_value,
             baseline_consumer, write_pending ? 1 : 0, restoring_system ? 'R' : 'A');
    return atomic_file(ROOT "/baseline", text, 0, 0);
}

static bool save_baseline(void) {
    if (owned) return true;
    if (!settle_inflight()) return false;
    if (!setting_get(baseline)) return false;
    char captured[8];
    if (!configuration_value("snapshot", captured) || strcmp(captured, baseline)) return false;
    char consumer[64];
    if (!tracking_call("query", consumer, sizeof consumer)) return false;
    if (!strcmp(consumer, "tracking_config=0")) baseline_consumer = 0;
    else if (!strcmp(consumer, "tracking_config=1")) baseline_consumer = 1;
    else return false;
    if (baseline_consumer != atoi(baseline)) return false;
    memcpy(expected, baseline, sizeof expected);
    memcpy(previous_value, baseline, sizeof previous_value);
    write_pending = false;
    restoring_system = false;
    if (!journal()) return false;
    owned = true;
    return true;
}

static bool apply_setting(const char *wanted) {
    char actual[8];
    if (!setting_get(actual)) return false;
    if (strcmp(actual, expected)) return false;
    if (!strcmp(expected, wanted)) return !strcmp(actual, wanted);
    // Journal the intended write first. A crash before the write leaves the
    // original untouched; a crash after the write can undo exactly our value.
    char previous[8]; memcpy(previous, expected, sizeof previous);
    memcpy(previous_value, expected, sizeof previous_value);
    write_pending = true;
    snprintf(expected, sizeof expected, "%s", wanted);
    if (!journal()) { memcpy(expected, previous, sizeof expected); return false; }
    // Recheck after durable I/O; a visible ownership change is not our write.
    if (!setting_get(actual) || strcmp(actual, previous)) {
        memcpy(expected, previous, sizeof expected);
        write_pending = false;
        journal();
        return false;
    }
    return !strcmp(actual, wanted) || setting_set(wanted, previous);
}

static bool restore(void) {
    operation_deadline = 0; // Recovery has its own bounded subprocess timeouts.
    bool ok = settle_inflight();
    if (owned) {
        char actual[8];
        char paired[8];
        bool pair_ok = configuration_value("baseline", paired) && !strcmp(paired, baseline);
        // Restore only our last intended value; preserve an external change.
        ok = ok && pair_ok && configuration_value("raw", actual);
        bool ours = ok && (!strcmp(actual, expected) || !strcmp(actual, baseline) ||
                          (write_pending && !strcmp(actual, previous_value)));
        if (ours) {
            restoring_system = true;
            ok = journal();
            if (ok) ok = setting_set(baseline, actual);
            const char *consumer = baseline_consumer ? "1" : "0";
            if (ok && !tracking_config(consumer)) {
                char repaired[64];
                ok = configuration_call("renotify-baseline", NULL, repaired, sizeof repaired) && tracking_config(consumer);
            }
            if (ok) ok = tracking_call(baseline_consumer ? "start" : "stop", NULL, 0);
            if (ok) {
                char final_value[8];
                ok = configuration_value("verify", final_value) && !strcmp(final_value, baseline) && tracking_config(consumer);
            }
        }
    }
    mode = 0; deadline = 0;
    if (ok && owned) {
        if (unlink(ROOT "/baseline") != 0 && errno != ENOENT) ok = false;
        int dir = open(ROOT, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (dir < 0 || fsync(dir)) ok = false;
        if (dir >= 0) close(dir);
        if (ok) owned = false;
    }
    restore_pending = !ok;
    if (ok) atomic_file(ROOT "/lease", "0\n", 0, 0);
    return ok;
}

static bool recover_if_owned(void) {
    return (!owned && mode == 0 && !restore_pending) || restore();
}

static bool apply(int requested, uint64_t lease) {
    uint64_t until = now_ms() + lease;
    bool transition = requested != mode;
    if (requested == 0) return restore();
    operation_deadline = until;
    if (owned && mode != 0) {
        char current[8];
        if (!configuration_value("verify", current) || strcmp(current, expected)) return false;
    }
    if (!save_baseline()) return false;
    char lease_record[32];
    snprintf(lease_record, sizeof lease_record, "%" PRIu64 "\n", until);
    if (!atomic_file(ROOT "/lease", lease_record, 0, 0)) return false;
    // Explicit hands is a temporary enable, even if the user's pre-session
    // setting was off. Restore/timeout still returns to that exact baseline.
    const char *wanted = requested == 1 ? "0" : "1";
    if (!apply_setting(wanted) || !tracking_config(wanted) ||
        ((requested == 1 || transition) && !tracking_call(requested == 1 ? "stop" : "start", NULL, 0)) ||
        now_ms() >= until) return false;
    write_pending = false;
    if (!journal()) return false;
    mode = requested; deadline = until;
    operation_deadline = 0;
    return true;
}

static bool valid_hex(const char *p, size_t n) {
    if (strlen(p) != n) return false;
    for (size_t i = 0; i < n; ++i)
        if (!((p[i] >= '0' && p[i] <= '9') || (p[i] >= 'a' && p[i] <= 'f'))) return false;
    return true;
}

static bool same_token(const char *a) {
    unsigned diff = 0;
    if (strlen(a) != 64) return false;
    for (size_t i = 0; i < 64; ++i) diff |= (unsigned char)a[i] ^ (unsigned char)token[i];
    return diff == 0;
}

static bool decimal_u64(const char *text, uint64_t *value) {
    if (!*text) return false;
    for (const char *p = text; *p; ++p) if (*p < '0' || *p > '9') return false;
    errno = 0;
    char *end = NULL;
    uint64_t parsed = strtoull(text, &end, 10);
    if (errno || !end || *end) return false;
    *value = parsed;
    return true;
}

static void reply(int fd, uint64_t seq, bool ok) {
    char setting[8] = "error";
    setting_get(setting);
    char text[256];
    // settings readback is deliberately not called a verified algorithm stop.
    int n = snprintf(text, sizeof text, "VDR1 %" PRIu64 " %d %s %s %s\n",
                      seq, mode, setting, "unobserved", ok ? "ok" : "error");
    send(fd, text, (size_t)n, MSG_NOSIGNAL);
}

#include "local_files.h"
#include "local_control.h"

static int serve(void) {
    int server = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (server < 0) return 1;
    int yes = 1; setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(PORT) };
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(server, (struct sockaddr *)&addr, sizeof addr) || listen(server, 1)) { close(server); return 1; }
    char ready[32];
    snprintf(ready, sizeof ready, "%d\n", getppid());
    if (!atomic_file(ROOT "/ready", ready, 0, 0)) { close(server); return 1; }
    int client = -1;
    char input[256], session[33] = {0};
    size_t used = 0;
    uint64_t seq_last = 0, client_until = 0, refresh = 0, last_control = 0;
    bool recovery_failed = false;
    while (!stopping) {
        uint64_t t = now_ms();
        if (access(ROOT "/enabled", F_OK)) break;
        if ((deadline && t >= deadline) || (client >= 0 && t >= client_until)) {
            if (client >= 0) close(client);
            client = -1; used = 0; session[0] = 0; seq_last = 0;
            if (local_control.until) local_drop(&recovery_failed);
            else recovery_failed = !recover_if_owned();
        }
        if ((restore_pending || (owned && mode == 0)) && t >= refresh) { refresh = t + 1000; recovery_failed = !restore(); }
        local_tick(client, &recovery_failed, &last_control);
        if (mode == 1 && deadline && t >= refresh && t - last_control >= 750) {
            refresh = t + 500;
            char current[8];
            operation_deadline = deadline;
            // A changed setting is an ownership conflict, not proof of an
            // automatic algorithm restart. Do not erase user/external edits.
            if (!setting_get(current) || strcmp(current, "0") || !tracking_config("0") ||
                !tracking_call("stop", NULL, 0)) {
                if (local_control.until) local_drop(&recovery_failed);
                else recovery_failed = !restore();
                if (client >= 0) { close(client); client = -1; used = 0; }
            }
            operation_deadline = 0;
        }
        struct pollfd pf[2] = {{server, POLLIN, 0}, {client, POLLIN, 0}};
        int rc = poll(pf, 2, 100);
        if (rc < 0) { if (errno == EINTR) continue; break; }
        if (pf[0].revents & POLLIN) {
            int incoming = accept4(server, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
            if (incoming >= 0) {
                if (client >= 0 || local_control.until || recovery_failed) close(incoming);
                else { client = incoming; client_until = now_ms() + 1500; used = 0; seq_last = 0; session[0] = 0; }
            }
        }
        if (client < 0) continue;
        if (pf[1].revents & (POLLHUP | POLLERR | POLLNVAL)) {
            close(client); client = -1; used = 0; recovery_failed = !recover_if_owned(); continue;
        }
        if (!(pf[1].revents & POLLIN)) continue;
        ssize_t n = recv(client, input + used, sizeof input - 1 - used, 0);
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) continue;
        if (n <= 0) { close(client); client = -1; used = 0; recovery_failed = !recover_if_owned(); continue; }
        used += (size_t)n; input[used] = 0;
        char *newline;
        while ((newline = memchr(input, '\n', used))) {
            if ((deadline && now_ms() >= deadline) || now_ms() >= client_until) {
                close(client); client = -1; used = 0; recovery_failed = !recover_if_owned(); break;
            }
            size_t length = (size_t)(newline - input);
            *newline = 0;
            char key[65], sid[33], command[16], sequence_text[21], lease_text[21];
            uint64_t seq = 0, lease = 0;
            int end = 0;
            bool valid = !memchr(input, 0, length) &&
                sscanf(input, "VDM1 %64s %32s %20s %15s %20s %n", key, sid, sequence_text, command, lease_text, &end) == 5 &&
                !input[end] && decimal_u64(sequence_text, &seq) && decimal_u64(lease_text, &lease) && same_token(key) && valid_hex(sid, 32) &&
                lease >= 500 && lease <= MAX_LEASE && seq > seq_last &&
                (!session[0] || !strcmp(session, sid));
            bool ok = false;
            if (valid) {
                if (!session[0]) memcpy(session, sid, sizeof session);
                seq_last = seq;
                if (!strcmp(command, "index")) { ok = apply(1, lease); if (ok) last_control = now_ms(); }
                else if (!strcmp(command, "hands")) { ok = apply(2, lease); if (ok) last_control = now_ms(); }
                else if (!strcmp(command, "restore")) ok = restore();
                else if (!strcmp(command, "status")) ok = true;
                client_until = now_ms() + lease;
                if (!ok && strcmp(command, "status")) recovery_failed = !recover_if_owned();
            }
            reply(client, seq, ok);
            memmove(input, newline + 1, used - length - 1); used -= length + 1;
            if (!valid || !ok) { close(client); client = -1; used = 0; recovery_failed = !recover_if_owned(); break; }
        }
        if (used == sizeof input - 1) { close(client); client = -1; used = 0; recovery_failed = !recover_if_owned(); }
    }
    if (client >= 0) close(client);
    close(server);
    unlink(ROOT "/ready");
    bool ok = restore();
    local_shutdown_reply();
    return ok ? 0 : 2;
}

static int supervise(void) {
    int lock = open(ROOT "/supervisor.lock", O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB)) return 1;
    unlink(ROOT "/ready");
    if (!atomic_file(ROOT "/lease", "0\n", 0, 0)) { close(lock); return 1; }
    char pid_record[32];
    snprintf(pid_record, sizeof pid_record, "%d\n", getpid());
    if (!atomic_file(ROOT "/pid", pid_record, 0, 0)) { close(lock); return 1; }
    if (!backend_start()) { close(lock); return 1; } // Parent owns bridge across worker death.
    pid_t parent = getpid();
    pid_t child = fork();
    if (child < 0) { backend_stop(); close(lock); return 1; }
    if (!child) {
        if (prctl(PR_SET_PDEATHSIG, SIGTERM) || getppid() != parent) _exit(1);
        export_backend();
        char *args[] = {"vdhs-mixedvrd", "--serve", NULL};
        execv("/proc/self/exe", args); _exit(127);
    }
    signal(SIGTERM, on_signal); signal(SIGINT, on_signal);
    int status = 0; bool reaped = false;
    uint64_t last_deadline = 0;
    unsigned lease_errors = 0;
    uint64_t startup_until = now_ms() + 10000;
    while (!stopping) {
        if (access(ROOT "/enabled", F_OK)) break;
        pid_t result = waitpid(child, &status, WNOHANG);
        if (result == child) { reaped = true; break; }
        if (result < 0 && errno != EINTR) break;
        char lease_record[32];
        bool valid_lease = false;
        if (read_small(ROOT "/lease", lease_record, sizeof lease_record, true) &&
            lease_record[0] >= '0' && lease_record[0] <= '9') {
            char *end = NULL;
            errno = 0;
            uint64_t until = strtoull(lease_record, &end, 10);
            if (!errno && end && !*end && until <= now_ms() + MAX_LEASE) {
                last_deadline = until;
                valid_lease = true;
            }
        }
        lease_errors = valid_lease ? 0 : lease_errors + 1;
        if (lease_errors >= 3 || (last_deadline && now_ms() >= last_deadline + 250)) break;
        if (now_ms() >= startup_until && access(ROOT "/ready", F_OK) != 0) break;
        struct timespec delay = {0, 100000000}; nanosleep(&delay, NULL);
    }
    unlink(ROOT "/ready"); // Worker unavailable: do not advertise readiness during recovery.
    if (!reaped) {
        kill(child, SIGTERM);
        uint64_t until = now_ms() + 4000;
        while (now_ms() < until) {
            if (waitpid(child, &status, WNOHANG) == child) { reaped = true; break; }
            struct timespec delay = {0, 50000000}; nanosleep(&delay, NULL);
        }
        if (!reaped) { kill(child, SIGKILL); while (waitpid(child, &status, 0) < 0 && errno == EINTR) {} }
    }
    // A distinct process performs recovery after worker crash/hang. A bounded
    // retry keeps the journal available if settings or app data are unavailable.
    int rc = 2;
    for (int attempt = 0; attempt < 3; ++attempt) {
        int bridge_status = 0;
        pid_t bridge_result = waitpid(backend_pid, &bridge_status, WNOHANG);
        if (bridge_result == backend_pid || (bridge_result < 0 && errno == ECHILD)) {
            // The worker is already gone, so no more requests can be issued.
            // Never replace an unknown in-flight service-side write. Once the
            // durable marker is absent, a dead bridge is just a transport fault.
            backend_pid = -1; // Reaped child: never signal a potentially reused PID.
            backend_stop();
            if (access(ROOT "/inflight", F_OK) == 0 || errno != ENOENT || !backend_start()) break;
        }
        pid_t recovery = fork();
        if (recovery == 0) {
            export_backend();
            char *args[] = {"vdhs-mixedvrd", "--restore", NULL};
            execv("/proc/self/exe", args); _exit(127);
        }
        if (recovery < 0) break;
        int recovery_status = 0;
        while (waitpid(recovery, &recovery_status, 0) < 0 && errno == EINTR) {}
        if (WIFEXITED(recovery_status) && WEXITSTATUS(recovery_status) == 0) { rc = 0; break; }
        sleep(1);
    }
    unlink(ROOT "/ready"); unlink(ROOT "/pid");
    backend_stop();
    close(lock);
    return rc;
}

int main(int argc, char **argv) {
    if (getuid() != 0 || argc != 2 ||
        (strcmp(argv[1], "--serve") && strcmp(argv[1], "--supervise") && strcmp(argv[1], "--restore") && strcmp(argv[1], "--status"))) {
        fprintf(stderr, "root required: vdhs-mixedvrd --supervise|--serve|--restore|--status\n"); return 1;
    }
    signal(SIGPIPE, SIG_IGN);
    if (mkdir(ROOT, 0700) && errno != EEXIST) return 1;
    char executable[512];
    ssize_t size = readlink("/proc/self/exe", executable, sizeof executable - 1);
    if (size > 0) {
        executable[size] = 0;
        char *slash = strrchr(executable, '/');
        if (slash) {
            *slash = 0;
            snprintf(tracking_jar, sizeof tracking_jar, "%s/tracking-control.jar", executable);
            *slash = '/'; // Preserve the full executable identity for worker validation.
        }
    }
    struct stat st;
    if (lstat(ROOT, &st) || !S_ISDIR(st.st_mode) || st.st_uid != 0 || (st.st_mode & 0077)) return 1;
    const char *pipe_in = getenv("VDHS_PIPE_IN"), *pipe_out = getenv("VDHS_PIPE_OUT"), *pipe_pid = getenv("VDHS_PIPE_PID"), *pipe_token = getenv("VDHS_PIPE_TOKEN");
    if (pipe_in && pipe_out && pipe_pid && pipe_token) {
        backend_in = atoi(pipe_in); backend_out = atoi(pipe_out); backend_pid = atoi(pipe_pid);
        backend_token = strtoull(pipe_token, NULL, 10);
        if (backend_in < 3 || backend_out < 3 || backend_pid <= 0 || !backend_token || fcntl(backend_in, F_GETFD) < 0 || fcntl(backend_out, F_GETFD) < 0) return 1;
        backend_shared = true;
    }
    if (!strcmp(argv[1], "--supervise")) return supervise();
    if (!strcmp(argv[1], "--serve")) {
        char parent_pid[32], parent_link[64], parent_exe[512];
        snprintf(parent_link, sizeof parent_link, "/proc/%d/exe", getppid());
        ssize_t parent_size = readlink(parent_link, parent_exe, sizeof parent_exe - 1);
        if (!read_small(ROOT "/pid", parent_pid, sizeof parent_pid, true) ||
            atoi(parent_pid) != getppid() || parent_size <= 0) return 1;
        parent_exe[parent_size] = 0;
        if (strcmp(parent_exe, executable)) return 1;
    }
    int lock = open(ROOT "/lock", O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB)) { fprintf(stderr, "helper already running\n"); return 1; }
    if (access(ROOT "/baseline", F_OK) == 0) {
        char record[96]; int end = 0; char phase = 0; int pending = 0;
        if (!read_small(ROOT "/baseline", record, sizeof record, true) ||
            sscanf(record, "VDB3 %7s %7s %7s %d %d %c %n", baseline, expected, previous_value, &baseline_consumer, &pending, &phase, &end) != 6 || record[end] ||
            (pending != 0 && pending != 1) || (strcmp(previous_value, "0") && strcmp(previous_value, "1")) ||
            (baseline_consumer != 0 && baseline_consumer != 1) || (phase != 'A' && phase != 'R') ||
            (strcmp(baseline, "0") && strcmp(baseline, "1")) ||
            (strcmp(expected, "0") && strcmp(expected, "1"))) {
            fprintf(stderr, "invalid baseline; manual recovery required\n"); close(lock); return 2;
        }
        restoring_system = phase == 'R';
        write_pending = pending != 0;
        owned = true;
    }
    if (!strcmp(argv[1], "--status")) {
        char value[8]; bool ok = setting_get(value);
        printf("configuration=%s recovery_pending=%d\n", ok ? value : "error", owned);
        char observed[64];
        printf("consumer=%s\n", tracking_call("query", observed, sizeof observed) ? observed : "unobserved");
        backend_stop(); close(lock); return ok ? 0 : 1;
    }
    if (!restore()) {
        fprintf(stderr, "baseline restore incomplete; journals retained. An uncertain Binder write in this boot requires the original bridge's completion barrier, or reboot before retrying --restore.\n");
        backend_stop(); close(lock); return 2;
    }
    if (!strcmp(argv[1], "--restore")) { backend_stop(); close(lock); return 0; }
    char token_file[68];
    if (!read_small(ROOT "/token", token_file, sizeof token_file, true) || !valid_hex(token_file, 64)) {
        fprintf(stderr, "root-owned 0600 token file (64 lowercase hex digits) required\n"); close(lock); return 1;
    }
    memcpy(token, token_file, sizeof token);
    if (access(ROOT "/enabled", F_OK)) { fprintf(stderr, "companion not enabled\n"); close(lock); return 1; }
    signal(SIGTERM, on_signal); signal(SIGINT, on_signal); signal(SIGPIPE, SIG_IGN);
    int rc = serve();
    backend_stop();
    close(lock);
    return rc;
}
