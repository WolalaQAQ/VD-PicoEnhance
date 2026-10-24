"""Execute production capability capture/companion and pre-server code offline."""
import ctypes
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]

MOCK = r'''
#include <stddef.h>
typedef long long ssize_t;
typedef void* jobject;
struct JNIEnv { bool ExceptionCheck() { return false; } void ExceptionClear() {} };
struct stat { unsigned st_mode, st_uid; long long st_size; };
struct pollfd { int fd; short events, revents; };
#define S_ISREG(m) (((m) & 0170000) == 0100000)
#define O_RDONLY 1
#define O_CLOEXEC 2
#define O_NOFOLLOW 4
#define AT_FDCWD -100
#define ENOENT 2
#define EINTR 4
#define EIO 5
#define EAGAIN 11
#define EACCES 13
#define EPIPE 32
#define ETIMEDOUT 110
#define POLLIN 1
#define POLLHUP 16
#define MSG_DONTWAIT 64
#define MSG_NOSIGNAL 16384
#define ANDROID_LOG_ERROR 6
#define EOF (-1)
static int errno, connects, direct_reads, root_reads, module_reads, allocations, releases;
static int violations, client_closes, callback_closes, polls, receives, sends, logs;
static int transport, directory_error, privileged, sent_byte, probe_blocked;
static bool opened[64];
struct Record { const unsigned char* bytes; size_t size, offset; int error; unsigned mode, uid; };
typedef Record FILE;
static Record files[6];
static unsigned char storage[1024];
extern "C" void* memset(void* p, int value, size_t n) {
    unsigned char* out = (unsigned char*)p;
    for (size_t i = 0; i < n; ++i) out[i] = (unsigned char)value;
    return p;
}
static int strcmp(const char* a, const char* b) {
    while (*a && *a == *b) { ++a; ++b; } return *a - *b;
}
static int memcmp(const void* a, const void* b, size_t n) {
    const unsigned char *x = (const unsigned char*)a, *y = (const unsigned char*)b;
    for (size_t i = 0; i < n; ++i) if (x[i] != y[i]) return x[i] - y[i];
    return 0;
}
static void* malloc(size_t n) { ++allocations; if (n > sizeof storage) ++violations; return storage; }
static void free(void*) { ++releases; }
static int close(int fd) {
    if (fd < 0 || fd >= 64 || !opened[fd]) { ++violations; return -1; }
    opened[fd] = false;
    if (fd == 51) ++client_closes;
    if (fd == 52 && privileged) ++callback_closes;
    return 0;
}
static int openat(int directory, const char* name, int flags) {
    if (flags != (O_RDONLY | O_CLOEXEC | O_NOFOLLOW)) ++violations;
    int index = -1;
    if (directory == AT_FDCWD && !strcmp(name, "/data/adb/vdhs-vd-capability/requested")) {
        if (!privileged) { ++direct_reads; errno = EACCES; return -1; }
        ++root_reads; index = 0;
    } else if (directory == 5) {
        if (!strcmp(name, "vd-capability.txt")) { ++module_reads; index = 1; }
        else if (!strcmp(name, "capability/classes.dex")) index = 2;
        else if (!strcmp(name, "payload/mode.txt")) index = 3;
        else if (!strcmp(name, "payload/libvdhs.so")) index = 4;
        else if (!strcmp(name, "payload/VdHsMod.dll")) index = 5;
    }
    if (index < 0) { ++violations; errno = ENOENT; return -1; }
    Record* record = &files[index];
    if (record->error) { errno = record->error; return -1; }
    int fd = 11 + index;
    if (opened[fd]) ++violations;
    opened[fd] = true; record->offset = 0;
    return fd;
}
static int fstat(int fd, struct stat* st) {
    Record* record = &files[fd - 11];
    st->st_mode = record->mode; st->st_uid = record->uid; st->st_size = (long long)record->size;
    return 0;
}
static ssize_t read(int fd, void* out, size_t n) {
    Record* record = &files[fd - 11];
    size_t left = record->size - record->offset;
    if (n > left) n = left;
    for (size_t i = 0; i < n; ++i) ((unsigned char*)out)[i] = record->bytes[record->offset++];
    return (ssize_t)n;
}
static FILE* fdopen(int fd, const char*) { return &files[fd - 11]; }
static int fgetc(FILE* record) { return record->offset == record->size ? EOF : record->bytes[record->offset++]; }
static int ferror(FILE*) { return 0; }
static int fclose(FILE* record) { return close(11 + (int)(record - files)); }
static int __android_log_print(int, const char*, const char*, ...) { ++logs; return 0; }
static ssize_t send(int fd, const void* bytes, size_t n, int flags) {
    ++sends;
    if (fd != 52 || n != 1 || flags != (MSG_NOSIGNAL | MSG_DONTWAIT) || !privileged) ++violations;
    if (transport == 8) { errno = EPIPE; return -1; }
    sent_byte = *(const unsigned char*)bytes;
    return 1;
}
static int poll(pollfd* wait, unsigned count, int timeout) {
    ++polls;
    if (count != 1 || wait->fd != 51 || wait->events != POLLIN || timeout != 1000) ++violations;
    if (transport == 2) return 0;
    if (transport == 6) { errno = EINTR; return -1; }
    wait->revents = sent_byte < 0 ? POLLHUP : POLLIN;
    return 1;
}
static ssize_t recv(int fd, void* bytes, size_t n, int flags) {
    ++receives;
    if (fd != 51 || n != 1 || flags != MSG_DONTWAIT) ++violations;
    if (transport == 7) { errno = EAGAIN; return -1; }
    if (sent_byte < 0) return 0;
    *(unsigned char*)bytes = (unsigned char)sent_byte;
    return 1;
}
'''

WRAPPER = r'''
struct FakeApi {
    int getModuleDir() {
        if (directory_error) return -1;
        opened[5] = true; return 5;
    }
    int connectCompanion() {
        ++connects;
        if (transport == 5) { errno = EACCES; return -1; }
        opened[51] = opened[52] = true;
        if (transport == 0 || transport == 8) {
            privileged = 1;
            vd_capability_companion(52);
            privileged = 0;
        } else if (transport == 3) sent_byte = 'X';
        else if (transport == 4) sent_byte = 'E';
        // Model the framework releasing its callback endpoint after the callback.
        close(52);
        return 51;
    }
};
struct Owner {
    FakeApi* api_;
    void pre() {
PRE_SERVER_BODY
    }
};
extern "C" __declspec(dllexport) void run(const unsigned char* saved, size_t ns,
        int saved_error, unsigned saved_mode, unsigned saved_uid,
        const unsigned char* module, size_t nm, int mode, int wire, int bad_payload, int bad_dex) {
    (void)retained_loader; (void)retained_bytes; (void)checked;
    dex_bytes = nullptr; dex_size = 0;
    connects = direct_reads = root_reads = module_reads = allocations = releases = violations = 0;
    client_closes = callback_closes = polls = receives = sends = logs = 0;
    privileged = 0; sent_byte = -1; errno = 0; transport = wire; directory_error = mode < 0;
    for (int i = 0; i < 64; ++i) opened[i] = false;
    static unsigned char dex[112] = {'d', 'e', 'x', '\n'};
    static unsigned char native[513] = {127, 'E', 'L', 'F', 2, 1};
    static unsigned char managed[513] = {'M', 'Z'};
    static unsigned char mode_text[2];
    native[18] = 183; native[19] = 0;
    mode_text[0] = (unsigned char)('0' + mode); mode_text[1] = '\n';
    files[0] = {saved, ns, 0, saved_error, saved_mode, saved_uid};
    files[1] = {module, nm, 0, module ? 0 : ENOENT, 0100600, 0};
    files[2] = {dex, sizeof dex, 0, bad_dex ? EACCES : 0, 0100644, 0};
    files[3] = {mode_text, sizeof mode_text, 0, 0, 0100644, 0};
    files[4] = {native, sizeof native, 0, bad_payload ? EACCES : 0, 0100644, 0};
    files[5] = {managed, sizeof managed, 0, 0, 0100644, 0};
    char probe[32]; bool missing = true;
    probe_blocked = !small_file(AT_FDCWD, "/data/adb/vdhs-vd-capability/requested", probe, sizeof probe, &missing)
                    && !missing && errno == EACCES;
    direct_reads = 0;
    FakeApi api; Owner owner {&api}; owner.pre();
}
extern "C" __declspec(dllexport) int value(int index) {
    int live = 0;
    for (int i = 0; i < 64; ++i) if (opened[i]) ++live;
    int values[] = {dex_bytes != nullptr, requested, connects, direct_reads, root_reads,
        module_reads, live, violations, client_closes, callback_closes, polls, receives,
        sends, logs, sent_byte, probe_blocked};
    return values[index];
}
'''


@unittest.skipUnless(os.name == 'nt', 'freestanding Windows capability fixture')
class CapabilityCompanionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        parent = Path(tempfile.gettempdir()) / 'opencode'
        parent.mkdir(exist_ok=True)
        cls.temp = tempfile.TemporaryDirectory(prefix='vd-capability-companion-', dir=parent)
        cls.addClassCleanup(cls.temp.cleanup)
        output = Path(cls.temp.name)
        native = (ROOT / 'zygisk/jni/vd_capability.cpp').read_text(encoding='utf-8')
        functions = native[native.index('namespace {'):native.index('static void bootstrap(')]
        parser = (ROOT / 'mod/native/vdhs_config.h').read_text(encoding='utf-8').replace('#include <stdio.h>', '')
        module = (ROOT / 'zygisk/jni/module.cpp').read_text(encoding='utf-8')
        start = module.index('void preServerSpecialize(')
        body = module[module.index('{', start) + 1:module.index('\n    }', start)]
        (output / 'companion.cpp').write_text(MOCK + parser + functions + WRAPPER.replace('PRE_SERVER_BODY', body),
                                             encoding='utf-8', newline='\n')
        sdk = Path(os.environ.get('ANDROID_SDK_ROOT', Path.home() / '.cache/pico-android/sdk'))
        llvm = sdk / 'ndk/26.1.10909125/toolchains/llvm/prebuilt/windows-x86_64/bin'
        commands = [
            [str(llvm / 'clang++.exe'), '--target=x86_64-pc-windows-msvc', '-ffreestanding',
             '-fno-stack-protector', '-fno-builtin', '-fno-exceptions', '-fno-rtti',
             '-O2', '-Wall', '-Wextra', '-Werror', '-c', str(output / 'companion.cpp'),
             '-o', str(output / 'companion.obj')],
            [str(llvm / 'ld.lld.exe'), '-flavor', 'link', '/dll', '/noentry', '/nodefaultlib',
             '/machine:x64', '/out:' + str(output / 'companion.dll'), str(output / 'companion.obj')],
        ]
        for command in commands:
            result = subprocess.run(command, capture_output=True, timeout=60)
            if result.returncode:
                raise AssertionError(result.stdout.decode('utf-8') + result.stderr.decode('utf-8'))
        cls.lib = ctypes.CDLL(str(output / 'companion.dll'))
        cls.lib.run.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_int, ctypes.c_uint,
                               ctypes.c_uint, ctypes.c_char_p, ctypes.c_size_t] + [ctypes.c_int] * 4
        cls.lib.run.restype = None
        cls.lib.value.argtypes = [ctypes.c_int]
        cls.lib.value.restype = ctypes.c_int
        kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        kernel.FreeLibrary.argtypes = [ctypes.c_void_p]
        kernel.FreeLibrary.restype = ctypes.c_int
        cls.addClassCleanup(lambda: kernel.FreeLibrary(cls.lib._handle))

    def run_capture(self, *, saved=b'VDC2 1\n', error=0, permissions=0o100600, uid=0,
                    module=b'VDC2 0\n', mode=2, wire=0, bad_payload=0, bad_dex=0):
        if saved is None and not error:
            error = 2
        self.lib.run(saved, len(saved or b''), error, permissions, uid,
                     module, len(module or b''), mode, wire, bad_payload, bad_dex)
        result = [self.lib.value(i) for i in range(16)]
        self.assertEqual(result[3], 0, 'specialize code opened the absolute saved-request path')
        self.assertEqual(result[6:8], [0, 0], 'FD leak, duplicate close, unexpected path or flags')
        self.assertEqual(result[9], 0, 'handler closed its borrowed callback endpoint')
        self.assertEqual(result[15], 1, 'fixture did not reproduce zygote EACCES')
        return result

    def test_companion_reads_saved_on_despite_zygote_path_eacces(self):
        result = self.run_capture()
        self.assertEqual(result[:6], [1, 1, 1, 0, 1, 0])
        self.assertEqual(result[8], 1)
        self.assertEqual(result[14], ord('1'))

    def test_saved_off_overrides_module_on_without_reading_module_choice(self):
        result = self.run_capture(saved=b'VDC2 0\n', module=b'VDC2 1\n')
        self.assertEqual(result[:6], [1, 0, 1, 0, 1, 0])
        self.assertEqual(result[14], ord('0'))

    def test_true_missing_saved_uses_module_off_or_on_or_default(self):
        for module, requested in [(b'VDC2 0\n', 0), (b'VDC2 1\n', 1), (None, 1)]:
            with self.subTest(module=module):
                result = self.run_capture(saved=None, module=module)
                self.assertEqual(result[:2], [1, requested])
                self.assertEqual(result[5], 1)
                self.assertEqual(result[14], ord('M'))

    def test_companion_file_errors_and_untrusted_content_never_fall_back(self):
        for options in [dict(error=13), dict(error=40), dict(saved=b'invalid\n'),
                        dict(saved=b'VDC2 0'), dict(saved=b''), dict(uid=1000),
                        dict(permissions=0o100620)]:
            with self.subTest(options=options):
                result = self.run_capture(module=b'VDC2 1\n', **options)
                self.assertEqual(result[0], 0)
                self.assertEqual(result[5], 0)
                self.assertEqual(result[14], ord('E'))
                self.assertGreater(result[13], 0)

    def test_eof_timeout_invalid_error_connect_poll_recv_and_send_fail_closed(self):
        for wire in range(1, 9):
            with self.subTest(wire=wire):
                result = self.run_capture(wire=wire, module=b'VDC2 1\n')
                self.assertEqual(result[0], 0)
                self.assertEqual(result[5], 0)
                self.assertEqual(result[8], int(wire != 5))
                self.assertGreater(result[13], 0)
                if wire in [2, 6]:
                    self.assertEqual(result[11], 0)

    def test_mode1_invalid_directory_and_payload_failure_do_not_connect(self):
        for options in [dict(mode=1), dict(mode=0), dict(mode=-1), dict(bad_payload=1)]:
            with self.subTest(options=options):
                result = self.run_capture(**options)
                self.assertEqual(result[0], 0)
                self.assertEqual(result[2], 0)
                self.assertEqual(result[4], 0)

    def test_missing_saved_with_invalid_module_choice_stays_closed(self):
        result = self.run_capture(saved=None, module=b'VDC2 0')
        self.assertEqual(result[0], 0)
        self.assertEqual(result[8], 1)

    def test_dex_open_failure_releases_every_fd(self):
        result = self.run_capture(bad_dex=1)
        self.assertEqual(result[0], 0)
        self.assertEqual(result[8], 1)

    def test_registration_and_api_calls_stay_in_pre_server(self):
        module = (ROOT / 'zygisk/jni/module.cpp').read_text(encoding='utf-8')
        self.assertIn('REGISTER_ZYGISK_COMPANION(vd_capability_companion)', module)
        self.assertEqual(module.count('api_->connectCompanion()'), 1)
        pre = module[module.index('void preServerSpecialize('):module.index('void postServerSpecialize(')]
        self.assertIn('vd_capability_capture(directory, api_->connectCompanion())', pre)
        native = (ROOT / 'zygisk/jni/vd_capability.cpp').read_text(encoding='utf-8')
        self.assertNotIn('connectCompanion', native)


if __name__ == '__main__':
    unittest.main(verbosity=2)
