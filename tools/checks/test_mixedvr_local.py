"""Compile the production text reader/controller; fake OS calls and Binder only."""
import ctypes as C
import os
from pathlib import Path
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'work/no-adb-20261021/local-resume/host-build'
SID = 'a' * 32

CRT = r'''
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
void *memcpy(void *to, const void *from, size_t n) {
    unsigned char *d = to; const unsigned char *s = from;
    for (size_t i = 0; i < n; ++i) d[i] = s[i]; return to;
}
void *memmove(void *to, const void *from, size_t n) {
    unsigned char *d = to; const unsigned char *s = from;
    if (d < s) for (size_t i = 0; i < n; ++i) d[i] = s[i];
    else for (size_t i = n; i; --i) d[i-1] = s[i-1]; return to;
}
void *memset(void *to, int c, size_t n) {
    unsigned char *d = to; for (size_t i = 0; i < n; ++i) d[i] = (unsigned char)c; return to;
}
size_t strlen(const char *s) { size_t n = 0; while (s[n]) ++n; return n; }
int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *x = a, *y = b;
    for (size_t i = 0; i < n; ++i) if (x[i] != y[i]) return x[i] - y[i]; return 0;
}
void *memchr(const void *data, int c, size_t n) {
    const unsigned char *p = data;
    for (size_t i = 0; i < n; ++i) if (p[i] == (unsigned char)c) return (void*)(p + i);
    return NULL;
}
int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { ++a; ++b; } return (unsigned char)*a - (unsigned char)*b;
}
int strncmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        if (a[i] != b[i]) return (unsigned char)a[i] - (unsigned char)b[i];
        if (!a[i]) return 0;
    } return 0;
}
char *strcpy(char *to, const char *from) { memcpy(to, from, strlen(from) + 1); return to; }
int snprintf(char *out, size_t cap, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); size_t n = 0;
    while (*fmt) {
        if (*fmt != '%') { if (n + 1 < cap) out[n] = *fmt; ++n; ++fmt; continue; }
        ++fmt; unsigned width = 0;
        while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (unsigned)(*fmt++ - '0');
        if (*fmt == 's') {
            const char *s = va_arg(ap, const char *);
            while (*s) { if (n + 1 < cap) out[n] = *s; ++n; ++s; }
        } else {
            bool wide = *fmt == 'l'; if (wide) fmt += 2;
            unsigned base = *fmt == 'x' ? 16 : 10;
            uint64_t value;
            if (wide) value = va_arg(ap, uint64_t);
            else {
                int v = va_arg(ap, int);
                if (v < 0) { if (n + 1 < cap) out[n] = '-'; ++n; v = -v; }
                value = (unsigned)v;
            }
            char digits[32]; unsigned count = 0;
            do { digits[count++] = "0123456789abcdef"[value % base]; value /= base; } while (value);
            while (width > count) { if (n + 1 < cap) out[n] = '0'; ++n; --width; }
            while (count) { if (n + 1 < cap) out[n] = digits[count - 1]; ++n; --count; }
        }
        ++fmt;
    }
    if (cap) out[n < cap ? n : cap - 1] = 0;
    va_end(ap); return (int)n;
}
'''

MOCK = r'''
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
typedef unsigned uid_t;
typedef unsigned gid_t;
typedef long long ssize_t;
struct stat { unsigned st_mode, st_uid, st_gid, st_nlink; long long st_size; };
#define O_RDONLY 1
#define O_DIRECTORY 2
#define O_CLOEXEC 4
#define O_NOFOLLOW 8
#define O_NONBLOCK 16
#define O_CREAT 32
#define O_EXCL 64
#define O_WRONLY 128
#define S_ISREG(m) ((m) == 1)
#define S_ISDIR(m) ((m) == 2)
static char request_text[256], answer[256];
static unsigned fault;
static int request_size, process_alive, process_padding, label_ok;
static int opens, reads, writes, renames, unlinks, closes, unsafe, chowns;
static int tcp_closed;
static int applies, restores, apply_ok, restore_ok, observed;
static int mode, owned;
static bool restore_pending;
static uint64_t clock_ms, deadline;
static bool recovery_failed;
static uint64_t last_control;
static int local_process;
static struct stat fs(int fd) {
    struct stat st = {2, 10115, 10115, 1, 0};
    if (fd == 13) { st.st_mode = fault & 8 ? 2 : 1; st.st_size = request_size;
        if (fault & 16) st.st_uid = 10116;
        if (fault & 32) st.st_nlink = 2;
        if (fault & 64) st.st_size = 128;
    }
    if (fd == 11 && (fault & 128)) st.st_uid = 0;
    if (fd == 12 && (fault & 256)) st.st_uid = 10116;
    if (fd == 20 && (fault & 512)) st.st_uid = 10116;
    return st;
}
static int fake_open(const char *path, int flags) {
    ++opens;
    if (strcmp(path, "/data/data")) { ++unsafe; return -1; }
    if ((flags & (O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC)) != (O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC)) ++unsafe;
    return fault & 1 ? -1 : 10;
}
static int fake_openat(int fd, const char *name, int flags, ...) {
    ++opens;
    if (!(flags & O_NOFOLLOW) || !(flags & O_CLOEXEC)) ++unsafe;
    if (fd == 10 && !strcmp(name, "VirtualDesktop.Android")) return fault & 2 ? -1 : 11;
    if (fd == 11 && !strcmp(name, "vdhs")) return fault & 4 ? -1 : 12;
    if (fd == 12 && !strcmp(name, "mixedvr_request.txt")) {
        if (!(flags & O_NONBLOCK)) ++unsafe;
        return fault & 1024 ? -1 : 13;
    }
    if (fd == 30 && !strcmp(name, "42")) return 20;
    if (fd == 20 && !strcmp(name, "cmdline")) return process_alive ? 21 : -1;
    if (fd == 12 && !strncmp(name, ".mixedvr_status.", 16)) {
        if ((flags & (O_CREAT|O_EXCL|O_WRONLY)) != (O_CREAT|O_EXCL|O_WRONLY)) ++unsafe;
        return fault & 2048 ? -1 : 14;
    }
    ++unsafe; return -1; // In particular, never open/truncate mixedvr_status.txt.
}
static int fake_fstat(int fd, struct stat *st) { *st = fs(fd); return 0; }
static ssize_t fake_read(int fd, void *out, size_t cap) {
    ++reads;
    if (fd == 21) {
        const char main_name[] = "VirtualDesktop.Android\0\0";
        const char other[] = "VirtualDesktop.Android:service";
        size_t n = fault & 4096 ? sizeof other : sizeof main_name - (process_padding ? 0 : 2);
        const char *p = fault & 4096 ? other : main_name;
        if (n > cap) n = cap;
        memcpy(out, p, n); return (ssize_t)n;
    }
    if (fd == 13) {
        size_t n = (size_t)request_size;
        if (fault & 8192) --n;
        if (n > cap) n = cap;
        memcpy(out, request_text, n); return (ssize_t)n;
    }
    return -1;
}
static int fake_close(int fd) { if (fd == 99) ++tcp_closed; ++closes; return 0; }
static ssize_t fake_random(void *out, size_t cap, unsigned flags) {
    (void)flags;
    memset(out, 0x35, cap); return fault & 16384 ? -1 : (ssize_t)cap;
}
static ssize_t fake_getxattr(int fd, const char *name, void *out, size_t cap) {
    if (fd != 12 || strcmp(name, "security.selinux") || cap < 8) ++unsafe;
    memcpy(out, "app:MCS", 8); return label_ok ? 8 : -1;
}
static int fake_setxattr(int fd, const char *name, const void *text, size_t size, int flags) {
    if (fd != 14 || strcmp(name, "security.selinux") || size != 8 || memcmp(text, "app:MCS", 8) || flags) ++unsafe;
    return fault & 32768 ? -1 : 0;
}
static int fake_chmod(int fd, unsigned bits) {
    if (fd != 14 || bits != 0600) ++unsafe;
    return fault & 65536 ? -1 : 0;
}
static int fake_chown(int fd, unsigned uid, unsigned gid) {
    ++chowns;
    if (fd != 14 || uid != 10115 || gid != 10115) ++unsafe;
    return fault & 131072 ? -1 : 0;
}
static bool write_all(int fd, const char *p, size_t n) {
    ++writes;
    if (fd != 14) ++unsafe;
    if (fault & 262144) return false;
    memcpy(answer, p, n); answer[n] = 0; return true;
}
static int fake_renameat(int from, const char *tmp, int to, const char *name) {
    ++renames;
    if (from != 12 || to != 12 || strncmp(tmp, ".mixedvr_status.", 16) || strcmp(name, "mixedvr_status.txt")) ++unsafe;
    return fault & 524288 ? -1 : 0;
}
static int fake_unlinkat(int fd, const char *name, int flags) {
    ++unlinks;
    if (fd != 12 || strncmp(name, ".mixedvr_status.", 16) || flags) ++unsafe;
    return 0;
}
#define open fake_open
#define openat fake_openat
#define fstat fake_fstat
#define read fake_read
#define close fake_close
#define getrandom fake_random
#define fgetxattr fake_getxattr
#define fsetxattr fake_setxattr
#define fchmod fake_chmod
#define fchown fake_chown
#define renameat fake_renameat
#define unlinkat fake_unlinkat
'''

MOCK_END = r'''
static uint64_t now_ms(void) { return clock_ms; }
static bool setting_get(char out[8]) {
    if (!observed) return false;
    strcpy(out, mode == 1 ? "0" : "1"); return true;
}
static bool restore(void) {
    ++restores; mode = 0; deadline = 0;
    restore_pending = !restore_ok;
    if (restore_ok) owned = 0;
    return restore_ok;
}
static bool recover_if_owned(void) {
    return (!owned && mode == 0 && !restore_pending) || restore();
}
static bool apply(int wanted, uint64_t lease) {
    ++applies;
    if (lease != 3000) ++unsafe;
    owned = 1;
    if (!apply_ok) return false;
    mode = wanted; deadline = now_ms() + lease; return true;
}
#include "local_control.h"
#define API __declspec(dllexport)
API void reset(void) {
    memset(&local_control, 0, sizeof local_control);
    memset(answer, 0, sizeof answer);
    local_process = -1;
    fault = 0; request_size = 0; process_alive = 1; process_padding = 0; label_ok = 1;
    opens = reads = writes = renames = unlinks = closes = unsafe = chowns = 0;
    applies = restores = mode = owned = 0; apply_ok = restore_ok = observed = 1;
    deadline = 0; clock_ms = 10000; recovery_failed = restore_pending = false;
    last_control = 0;
}
API void put(const char *text, int size) { memcpy(request_text, text, (size_t)size); request_size = size; }
API void faults(unsigned value) { fault = value; }
API void options(int alive, int padding, int label, int aok, int rok, int obs) {
    process_alive = alive; process_padding = padding; label_ok = label;
    apply_ok = aok; restore_ok = rok; observed = obs;
}
API void tick(int client, uint64_t time) { clock_ms = time; local_tick(client, &recovery_failed, &last_control); }
API int parse(const char *text, int size) { struct local_request r; return local_parse(text, (size_t)size, &r); }
API uint64_t value(int which) {
    switch (which) {
    case 0: return mode; case 1: return applies; case 2: return restores;
    case 3: return local_control.until; case 4: return deadline;
    case 5: return local_control.sequence; case 6: return local_control.ready;
    case 7: return unsafe; case 8: return renames; case 9: return unlinks;
    case 10: return writes; case 11: return recovery_failed; case 12: return chowns;
    }
    return 0;
}
API const char *reply_text(void) { return answer; }
API void shutdown_local(void) { restore(); local_shutdown_reply(); }
'''

LOOP_MOCK = r'''
#define ROOT "/data/adb/vdhs_mixedvr"
#define PORT 29743
#define MAX_LEASE 5000
#define AF_INET 1
#define SOCK_STREAM 2
#define SOCK_CLOEXEC 4
#define SOCK_NONBLOCK 8
#define SOL_SOCKET 1
#define SO_REUSEADDR 2
#define INADDR_LOOPBACK 1
#define POLLIN 1
#define POLLHUP 2
#define POLLERR 4
#define POLLNVAL 8
#define F_OK 0
#define EINTR 1
#define EAGAIN 2
#define errno fake_errno
struct sockaddr { int unused; };
struct sockaddr_in { int sin_family, sin_port; struct { int s_addr; } sin_addr; };
struct pollfd { int fd, events, revents; };
static int stopping, fake_errno, scenario, steps, helper_enabled, tcp_accepted;
static int snapshot_mode, snapshot_restores, ticks_after_restore;
static uint64_t operation_deadline;
static int htons(int n) { return n; }
static int htonl(int n) { return n; }
static int socket(int domain, int flags, int protocol) { (void)domain; (void)flags; (void)protocol; return 90; }
static int setsockopt(int fd, int level, int name, const void *value, size_t size) {
    (void)fd; (void)level; (void)name; (void)value; (void)size; return 0;
}
static int bind(int fd, const struct sockaddr *addr, size_t size) { (void)fd; (void)addr; (void)size; return 0; }
static int listen(int fd, int backlog) { (void)fd; (void)backlog; return 0; }
static int getppid(void) { return 1; }
static bool atomic_file(const char *path, const char *text, uid_t uid, gid_t gid) {
    (void)path; (void)text; (void)uid; (void)gid; return true;
}
static int access(const char *path, int flags) {
    if (strcmp(path, ROOT "/enabled") || flags) ++unsafe;
    return helper_enabled ? 0 : -1;
}
static int unlink(const char *path) { if (strcmp(path, ROOT "/ready")) ++unsafe; return 0; }
static bool tracking_config(const char *wanted) { (void)wanted; return true; }
static bool tracking_call(const char *command, char *out, size_t cap) { (void)command; (void)out; (void)cap; return true; }
static int accept4(int fd, void *a, void *b, int flags) { (void)fd; (void)a; (void)b; (void)flags; ++tcp_accepted; return 99; }
static ssize_t recv(int fd, void *out, size_t size, int flags) { (void)fd; (void)out; (void)size; (void)flags; return 0; }
// No TCP payload parsing in these loop tests: its read event is EOF only.
static int sscanf(const char *text, const char *fmt, ...) { (void)text; (void)fmt; ++unsafe; return 0; }
static bool decimal_u64(const char *text, uint64_t *out) { (void)text; (void)out; return false; }
static bool valid_hex(const char *text, size_t size) { (void)text; (void)size; return false; }
static bool same_token(const char *text) { (void)text; return false; }
static void reply(int fd, uint64_t seq, bool ok) { (void)fd; (void)seq; (void)ok; ++unsafe; }
static void loop_request(int seq, const char *command) {
    snprintf(request_text, sizeof request_text, "VDL1 aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa %d %s\n", seq, command);
    request_size = (int)strlen(request_text);
}
static void loop_stop(void) { snapshot_mode = mode; snapshot_restores = restores; stopping = 1; }
static int poll(struct pollfd *pf, int count, int timeout) {
    if (count != 2 || timeout != 100) ++unsafe;
    pf[0].revents = pf[1].revents = 0;
    ++steps;
    if (steps > 10) { ++unsafe; stopping = 1; return 0; }
    if (scenario == 0) { // status extends owner, not the mode deadline.
        if (steps == 1) { clock_ms = 10100; loop_request(2, "index"); }
        if (steps == 2) { clock_ms = 12000; loop_request(3, "status"); }
        if (steps == 3) clock_ms = 13100;
        if (steps == 4) loop_stop();
    } else if (scenario == 1) { // Local reservation rejects every TCP accept.
        if (steps <= 2) pf[0].revents = POLLIN;
        if (steps == 2) { clock_ms = 10100; loop_request(2, "index"); }
        if (steps == 3) loop_stop();
    } else if (scenario == 2) { // TCP first; consume local records, then fresh handshake.
        if (steps == 1) { fault = 0; pf[0].revents = POLLIN; clock_ms = 10100; }
        if (steps == 2) { pf[1].revents = POLLIN; clock_ms = 10200; loop_request(2, "index"); }
        if (steps == 3) { clock_ms = 10300; loop_request(3, "status"); }
        if (steps == 4) { clock_ms = 10400; loop_request(4, "index"); }
        if (steps == 5) loop_stop();
    } else if (scenario == 3) { // Bad local file must not restore TCP's mode.
        if (steps == 1) {
            fault = 0; pf[0].revents = POLLIN;
            mode = 1; owned = 1; deadline = 15000;
            memcpy(request_text, "bad\n", 4); request_size = 4;
        }
        if (steps == 2) loop_stop();
    } else if (scenario == 4) { // Disable is checked by the real loop.
        if (steps == 1) { clock_ms = 10100; loop_request(2, "index"); }
        if (steps == 2) { helper_enabled = 0; clock_ms = 10200; }
    } else if (scenario == 5) { // Exit restores while the request remains readable.
        if (steps == 1) { clock_ms = 10100; loop_request(2, "index"); }
        if (steps == 2) { process_alive = 0; clock_ms = 10200; }
        if (steps == 3) loop_stop();
    } else if (scenario == 6) { // Expired local reservation permits debug owner.
        if (steps == 1) clock_ms = 13000;
        if (steps == 2) pf[0].revents = POLLIN;
        if (steps == 3) { if (pf[1].fd != 99) ++unsafe; loop_stop(); }
    }
    if (!mode && restores) ++ticks_after_restore;
    return 1;
}
'''

LOOP_END = r'''
API int run_loop(int which) {
    reset(); scenario = which; steps = stopping = tcp_accepted = tcp_closed = 0;
    helper_enabled = 1; snapshot_mode = snapshot_restores = -1; ticks_after_restore = 0;
    loop_request(1, "status");
    if (which == 2 || which == 3) fault = 1024;
    return serve();
}
API int loop_value(int which) {
    switch (which) {
    case 0: return tcp_accepted; case 1: return tcp_closed;
    case 2: return snapshot_mode; case 3: return snapshot_restores;
    case 4: return ticks_after_restore; case 5: return steps;
    } return -1;
}
'''

DIRENT = r'''
struct dirent { char d_name[16]; };
typedef struct { int read; } DIR;
static DIR directory;
static DIR *opendir(const char *name) {
    if (strcmp(name, "/proc")) ++unsafe;
    directory.read = 0; return &directory;
}
static int dirfd(DIR *dir) { (void)dir; return 30; }
static struct dirent *readdir(DIR *dir) {
    static struct dirent entry = {"42"};
    return dir->read++ ? NULL : &entry;
}
static int closedir(DIR *dir) { (void)dir; return 0; }
'''


class LocalTextTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        OUT.mkdir(parents=True, exist_ok=True)
        (OUT / 'sys').mkdir(exist_ok=True)
        (OUT / 'dirent.h').write_text(DIRENT)
        (OUT / 'sys/xattr.h').write_text('/* fakes declared in harness */\n')
        # No production source is copied or rewritten; both headers are included.
        (OUT / 'stdio.h').write_text('#include <stddef.h>\nint snprintf(char*, size_t, const char*, ...);\n')
        (OUT / 'stdlib.h').write_text('/* no CRT imports */\n')
        (OUT / 'inttypes.h').write_text('#include <stdint.h>\n#define PRIu64 "llu"\n#define PRIx64 "llx"\n')
        (OUT / 'string.h').write_text('''#include <stddef.h>
void *memcpy(void*,const void*,size_t); void *memset(void*,int,size_t);
void *memmove(void*,const void*,size_t);
size_t strlen(const char*); int memcmp(const void*,const void*,size_t);
void *memchr(const void*,int,size_t);
int strcmp(const char*,const char*); int strncmp(const char*,const char*,size_t);
char *strcpy(char*,const char*);
''')
        source = MOCK.replace('static int local_process;\n', '')
        daemon = (ROOT / 'mixedvr/headset/mixedvrd.c').read_text()
        serve = daemon[daemon.index('static int serve(void)'):daemon.index('static int supervise(void)')]
        (OUT / 'serve_source.h').write_text(serve)
        (OUT / 'local_mock.c').write_text('#include <stdbool.h>\n' + CRT + source +
                                        '\n#include "local_files.h"\n' + MOCK_END + LOOP_MOCK +
                                        '\n#include "serve_source.h"\n' + LOOP_END)
        sdk = Path(os.environ.get('ANDROID_SDK_ROOT', Path.home() / '.cache/pico-android/sdk'))
        llvm = sdk / 'ndk/26.1.10909125/toolchains/llvm/prebuilt/windows-x86_64/bin'
        dll = OUT / f'local-{os.getpid()}.dll'
        obj = OUT / 'local_mock.obj'
        command = [str(llvm / 'clang.exe'), '--target=x86_64-pc-windows-msvc',
                   '-std=c11', '-ffreestanding', '-fno-builtin', '-fno-stack-protector',
                   '-O2', '-Wall', '-Wextra', '-Werror',
                   '-I', str(OUT), '-I', str(ROOT / 'mixedvr/headset'),
                   '-c', str(OUT / 'local_mock.c'), '-o', str(obj)]
        result = subprocess.run(command, capture_output=True, text=True, encoding='utf-8', timeout=120)
        (OUT / 'compile.log').write_text(' '.join(command) + '\n' + result.stdout + result.stderr)
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)
        command = [str(llvm / 'ld.lld.exe'), '-flavor', 'link', '/dll', '/noentry', '/nodefaultlib',
                   '/machine:x64', '/wx', '/out:' + str(dll), str(obj)]
        result = subprocess.run(command, capture_output=True, text=True, encoding='utf-8', timeout=30)
        (OUT / 'link.log').write_text(' '.join(command) + '\n' + result.stdout + result.stderr)
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)
        cls.lib = C.CDLL(str(dll))
        kernel = C.WinDLL('kernel32', use_last_error=True)
        kernel.FreeLibrary.argtypes, kernel.FreeLibrary.restype = [C.c_void_p], C.c_int
        def unload():
            if not kernel.FreeLibrary(cls.lib._handle):
                raise C.WinError(C.get_last_error())
        cls.addClassCleanup(unload)
        cls.lib.put.argtypes = [C.c_char_p, C.c_int]
        cls.lib.parse.argtypes = [C.c_char_p, C.c_int]
        cls.lib.tick.argtypes = [C.c_int, C.c_uint64]
        cls.lib.value.argtypes = [C.c_int]
        cls.lib.value.restype = C.c_uint64
        cls.lib.reply_text.restype = C.c_char_p

    def setUp(self):
        self.lib.reset()

    def tearDown(self):
        self.assertEqual(self.lib.value(7), 0, 'unsafe path/flags/identity or wrong fixed lease')

    def send(self, seq=1, command='status', sid=SID, *, time=10000, client=-1):
        self.put(f'VDL1 {sid} {seq} {command}\n'.encode())
        self.lib.tick(client, time)

    def put(self, text):
        self.lib.put(text, len(text))

    def option(self, *, alive=1, padding=0, label=1, apply=1, restore=1, observed=1):
        self.lib.options(alive, padding, label, apply, restore, observed)

    def armed(self):
        self.send()
        self.send(2, 'index', time=10100)
        self.assertEqual(self.lib.value(0), 1)

    def test_all_commands_and_max_u64_parse(self):
        for command in ('status', 'index', 'hands', 'restore'):
            text = f'VDL1 {SID} 18446744073709551615 {command}\n'.encode()
            self.assertTrue(self.lib.parse(text, len(text)))

    def test_reject_noncanonical_records(self):
        valid = f'VDL1 {SID} 1 status\n'.encode()
        invalid = [b'', valid[:-1], b'\xef\xbb\xbf' + valid, valid + b'\n',
                   valid.replace(b'\n', b'\r\n'), valid.replace(b' 1 ', b' 01 '),
                   valid.replace(b' 1 ', b' 0 '), valid.replace(b' 1 ', b' -1 '),
                   valid.replace(b' 1 ', b' +1 '), valid.replace(b' 1 ', b' 18446744073709551616 '),
                   valid.replace(b' 1 ', b' 1x '), valid.replace(b' 1 ', b'  1 '),
                   valid.replace(b' 1 ', b'\t1 '), valid.replace(b'status', b'STATUS'),
                   valid.replace(b'status', b'root'), valid.replace(b'aaaa', b'AAAA', 1),
                   valid.replace(b'aaaa', b'aaag', 1), valid.replace(b'aaaa', b'aaa', 1),
                   valid.replace(b'\n', b'\x00\n'), valid + b'x' * 128]
        for text in invalid:
            with self.subTest(text=text):
                self.assertFalse(self.lib.parse(text, len(text)))

    def test_first_mode_is_only_seed_not_replayed(self):
        self.send(9, 'index')
        self.assertEqual(self.lib.value(1), 0)
        self.assertIn(b' 9 0 1 error\n', self.lib.reply_text())

    def test_status_handshake_reserves_without_apply(self):
        self.send()
        self.assertEqual(self.lib.value(1), 0)
        self.assertEqual(self.lib.value(3), 13000)
        self.assertEqual(self.lib.reply_text(), f'VDRL1 {SID} 1 0 1 ok\n'.encode())

    def test_index_calls_existing_apply_with_fixed_lease(self):
        self.armed()
        self.assertEqual(self.lib.value(4), 13100)
        self.assertIn(b' 2 1 0 ok\n', self.lib.reply_text())

    def test_hands_and_restore_use_existing_endpoints(self):
        self.armed()
        self.send(3, 'hands', time=10200)
        self.assertEqual(self.lib.value(0), 2)
        self.send(4, 'restore', time=10300)
        self.assertEqual(self.lib.value(0), 0)
        self.assertEqual(self.lib.value(2), 1)
        self.assertEqual(self.lib.value(4), 0)

    def test_duplicate_and_old_seq_never_renew(self):
        self.armed()
        for seq in (2, 1, 2):
            self.send(seq, 'hands', time=12000)
        self.assertEqual(self.lib.value(1), 1)
        self.assertEqual(self.lib.value(3), 13100)
        self.assertEqual(self.lib.value(8), 2)

    def test_only_new_sequence_extends_mode_lease(self):
        self.armed()
        self.send(3, 'index', time=12000)
        self.assertEqual(self.lib.value(4), 15000)
        self.assertEqual(self.lib.value(1), 2)

    def test_status_does_not_renew_mode_deadline(self):
        self.armed()
        self.send(3, 'status', time=12000)
        self.assertEqual(self.lib.value(4), 13100)
        self.assertEqual(self.lib.value(3), 15000)

    def test_frozen_mode_expires_and_requires_status_again(self):
        self.armed()
        self.lib.tick(-1, 13100)
        self.assertEqual(self.lib.value(0), 0)
        self.assertEqual(self.lib.value(2), 1)
        self.assertEqual(self.lib.value(3), 0)
        self.send(3, 'hands', time=13200)
        self.assertEqual(self.lib.value(1), 1)
        self.send(4, 'status', time=13300)
        self.send(5, 'hands', time=13400)
        self.assertEqual(self.lib.value(0), 2)

    def test_missing_request_restores_immediately(self):
        self.armed()
        self.lib.faults(1024)
        self.lib.tick(-1, 10200)
        self.assertEqual(self.lib.value(0), 0)
        self.assertEqual(self.lib.value(3), 0)

    def test_invalid_record_restores_immediately(self):
        self.armed()
        self.put(b'invalid\n')
        self.lib.tick(-1, 10200)
        self.assertEqual(self.lib.value(0), 0)

    def test_return_of_old_record_after_loss_cannot_reapply(self):
        self.armed()
        self.lib.faults(1024)
        self.lib.tick(-1, 10200)
        self.lib.faults(0)
        self.send(2, 'index', time=10300)
        self.assertEqual(self.lib.value(0), 0)
        self.assertEqual(self.lib.value(1), 1)

    def test_dead_app_restores_despite_fresh_file(self):
        self.armed()
        self.option(alive=0)
        self.send(3, 'index', time=10200)
        self.assertEqual(self.lib.value(0), 0)
        self.assertEqual(self.lib.value(1), 1)

    def test_restarted_process_does_not_replay_old_file(self):
        self.armed()
        self.option(alive=0)
        self.lib.tick(-1, 10200)
        self.option()
        self.send(3, 'index', time=10300)
        self.assertEqual(self.lib.value(0), 0)

    def test_new_session_restores_then_handshakes(self):
        self.armed()
        self.send(1, 'status', sid='b'*32, time=10200)
        self.assertEqual(self.lib.value(0), 0)
        self.assertEqual(self.lib.value(6), 1)
        self.send(2, 'hands', sid='b'*32, time=10300)
        self.assertEqual(self.lib.value(0), 2)

    def test_tcp_owner_rejects_and_consumes_local_request(self):
        self.send(client=99)
        self.assertEqual(self.lib.value(3), 0)
        self.assertIn(b' error\n', self.lib.reply_text())
        self.send(1, time=10100)
        self.assertEqual(self.lib.value(3), 0)
        self.send(2, time=10200)
        self.assertEqual(self.lib.value(6), 1)

    def test_failed_apply_runs_restore_and_disarms(self):
        self.send()
        self.option(apply=0)
        self.send(2, 'index', time=10100)
        self.assertEqual(self.lib.value(2), 1)
        self.assertEqual(self.lib.value(3), 0)
        self.assertIn(b' error\n', self.lib.reply_text())

    def test_incomplete_restore_blocks_next_owner(self):
        self.armed()
        self.option(restore=0)
        self.send(3, 'restore', time=10200)
        self.assertEqual(self.lib.value(11), 1)
        self.send(4, 'status', time=10300)
        self.assertEqual(self.lib.value(3), 0)

    def test_unknown_readback_is_error_not_fabricated_config(self):
        self.send()
        self.option(observed=0)
        self.send(2, 'index', time=10100)
        self.assertIn(b' -1 error\n', self.lib.reply_text())
        self.assertEqual(self.lib.value(0), 0)

    def test_reply_failure_restores_active_mode(self):
        self.send()
        self.lib.faults(2048)
        self.send(2, 'index', time=10100)
        self.assertEqual(self.lib.value(0), 0)
        self.assertEqual(self.lib.value(2), 1)

    def test_expiry_error_publication_is_retried_after_transient_failure(self):
        self.armed()
        self.lib.faults(2048)
        self.lib.tick(-1, 13100)
        self.assertEqual(self.lib.value(0), 0)
        self.assertIn(b' 2 1 0 ok\n', self.lib.reply_text())
        self.lib.faults(0)
        self.lib.tick(-1, 13200)
        self.assertIn(b' 2 0 1 error\n', self.lib.reply_text())
        self.assertEqual(self.lib.value(1), 1)
        self.assertEqual(self.lib.value(3), 0)

    def test_shutdown_restore_publishes_mode_zero_error(self):
        self.armed()
        self.lib.shutdown_local()
        self.assertIn(b' 2 0 1 error\n', self.lib.reply_text())

    def test_app_process_cmdline_can_have_zero_padding(self):
        self.option(padding=1)
        self.send()
        self.assertIn(b' ok\n', self.lib.reply_text())

    def test_subprocess_name_is_not_main_app(self):
        self.lib.faults(4096)
        self.send()
        self.assertEqual(self.lib.value(8), 0)

    def test_symlink_components_are_not_followed(self):
        for fault in (1, 2, 4, 1024):
            with self.subTest(fault=fault):
                self.lib.reset()
                self.lib.faults(fault)
                self.send()
                self.assertEqual(self.lib.value(8), 0)

    def test_wrong_owners_nonregular_and_hardlinked_request_rejected(self):
        for fault in (8, 16, 32, 128, 256, 512):
            with self.subTest(fault=fault):
                self.lib.reset()
                self.lib.faults(fault)
                self.send()
                self.assertEqual(self.lib.value(8), 0)

    def test_oversized_and_torn_read_rejected(self):
        for fault in (64, 8192):
            self.lib.reset()
            self.lib.faults(fault)
            self.send()
            self.assertEqual(self.lib.value(8), 0)

    def test_temp_collision_does_not_unlink_or_write_existing_inode(self):
        self.lib.faults(2048)
        self.send()
        self.assertEqual(self.lib.value(9), 0)
        self.assertEqual(self.lib.value(10), 0)

    def test_random_failure_does_not_create_response(self):
        self.lib.faults(16384)
        self.send()
        self.assertEqual(self.lib.value(10), 0)

    def test_label_failure_never_publishes_wrong_context(self):
        self.option(label=0)
        self.send()
        self.assertEqual(self.lib.value(8), 0)
        self.assertEqual(self.lib.value(9), 1)
        self.assertEqual(self.lib.value(10), 0)

    def test_reply_write_chmod_chown_and_rename_failures_cleanup(self):
        for fault in (32768, 65536, 131072, 262144, 524288):
            with self.subTest(fault=fault):
                self.lib.reset()
                self.lib.faults(fault)
                self.send()
                self.assertEqual(self.lib.value(9), 1)
                self.assertEqual(self.lib.value(3), 0)

    def test_reply_new_inode_uses_app_identity_and_atomic_fixed_name(self):
        self.send()
        self.assertEqual(self.lib.value(12), 1)
        self.assertEqual(self.lib.value(8), 1)
        self.assertEqual(self.lib.value(9), 0)

    def test_production_loop_serializes_tcp_and_local_and_checks_disable(self):
        source = (ROOT / 'mixedvr/headset/mixedvrd.c').read_text()
        serve = source[source.index('static int serve(void)'):source.index('static int supervise(void)')]
        self.assertIn('local_tick(client, &recovery_failed, &last_control);', serve)
        self.assertIn('if (client >= 0 || local_control.until || recovery_failed) close(incoming);', serve)
        self.assertIn('if (access(ROOT "/enabled", F_OK)) break;', serve)
        self.assertIn('local_drop(&recovery_failed);', serve)
        self.assertLess(serve.index('bool ok = restore();'), serve.index('local_shutdown_reply();'))
        supervisor = source[source.index('static int supervise(void)'):source.index('int main(')]
        self.assertIn('if (access(ROOT "/enabled", F_OK)) break;', supervisor)

    def test_real_loop_status_cannot_keep_index_past_mode_expiry(self):
        self.assertEqual(self.lib.run_loop(0), 0)
        self.assertEqual(self.lib.loop_value(2), 0)
        self.assertEqual(self.lib.loop_value(3), 1)
        self.assertGreater(self.lib.loop_value(4), 0)

    def test_real_loop_local_owner_rejects_tcp_accepts(self):
        self.assertEqual(self.lib.run_loop(1), 0)
        self.assertEqual(self.lib.loop_value(0), 2)
        self.assertEqual(self.lib.loop_value(1), 2)
        self.assertEqual(self.lib.loop_value(2), 1)

    def test_real_loop_tcp_owner_then_eof_then_new_local_handshake(self):
        self.assertEqual(self.lib.run_loop(2), 0)
        self.assertEqual(self.lib.loop_value(0), 1)
        self.assertEqual(self.lib.loop_value(1), 1)
        self.assertEqual(self.lib.value(1), 1)
        self.assertEqual(self.lib.loop_value(2), 1)

    def test_real_loop_invalid_local_record_cannot_restore_tcp_owner(self):
        self.assertEqual(self.lib.run_loop(3), 0)
        self.assertEqual(self.lib.loop_value(2), 1)
        self.assertEqual(self.lib.loop_value(3), 0)

    def test_real_loop_disable_restores_before_return(self):
        self.assertEqual(self.lib.run_loop(4), 0)
        self.assertEqual(self.lib.value(0), 0)
        self.assertEqual(self.lib.value(2), 1)
        self.assertEqual(self.lib.loop_value(5), 2)

    def test_real_loop_app_exit_restores_without_waiting_for_lease(self):
        self.assertEqual(self.lib.run_loop(5), 0)
        self.assertEqual(self.lib.loop_value(2), 0)
        self.assertEqual(self.lib.loop_value(3), 1)

    def test_real_loop_expired_local_owner_allows_debug_connection(self):
        self.assertEqual(self.lib.run_loop(6), 0)
        self.assertEqual(self.lib.loop_value(0), 1)
        self.assertEqual(self.lib.loop_value(1), 1)


if __name__ == '__main__':
    unittest.main(verbosity=2)
