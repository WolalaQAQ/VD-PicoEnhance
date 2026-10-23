"""Execute production boot workers and shell scripts with offline host doubles."""
import ctypes
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from test_shell_policy import SH

ROOT = Path(__file__).resolve().parents[2]

MOCK = r'''
typedef unsigned long long size_t;
typedef long long ssize_t;
typedef int pthread_t;
typedef int pthread_attr_t;
#define JNI_OK 0
#define PROP_VALUE_MAX 92
#define ANDROID_LOG_INFO 4
#define ANDROID_LOG_ERROR 6
#define O_RDONLY 0
#define O_CLOEXEC 0
#define PTHREAD_CREATE_DETACHED 1
static const char* kTag = "test";
static int ticks, boot_at, named_at, attaches, detaches, bootstraps, stages, frees;
static int attach_error, create_error, vm_error, violations, created, started;
static unsigned char storage[16];
static unsigned char* dex_bytes;
static size_t dex_size;
static void* (*pending)(void*);
static void* pending_context;
static int strcmp(const char* a, const char* b) {
    while (*a && *a == *b) { ++a; ++b; } return *a - *b;
}
static void free(void*) { ++frees; }
static unsigned sleep(unsigned seconds) { ticks += (int)seconds; return 0; }
static int __system_property_get(const char*, char* value) {
    if (ticks < boot_at) { value[0] = '0'; value[1] = 0; return 1; }
    value[0] = '1'; value[1] = 0; return 1;
}
static int open(const char*, int) { return 7; }
static ssize_t read(int, void* buffer, size_t) {
    const char* name = ticks < named_at ? "zygote64" : "system_server";
    char* out = (char*)buffer; size_t n = 0;
    do { out[n] = name[n]; } while (name[n++]);
    return (ssize_t)n;
}
static int close(int) { return 0; }
static int __android_log_print(int, const char*, const char*, ...) { return 0; }
#define LOGI(...) ((void)0)
static pthread_t pthread_self() { return 1; }
static int pthread_setname_np(pthread_t, const char*) { return 0; }
static int pthread_attr_init(pthread_attr_t*) { return 0; }
static int pthread_attr_setdetachstate(pthread_attr_t*, int) { return 0; }
static int pthread_attr_destroy(pthread_attr_t*) { return 0; }
static int pthread_create(pthread_t*, pthread_attr_t*, void* (*entry)(void*), void* context) {
    ++created; pending = entry; pending_context = context; return create_error;
}
struct JNIEnv;
static JNIEnv* attached_env;
struct JavaVM {
    int AttachCurrentThreadAsDaemon(JNIEnv** env, void*) {
        ++attaches;
        if (ticks < boot_at || ticks < named_at) ++violations;
        *env = attached_env; return attach_error;
    }
    int DetachCurrentThread() { ++detaches; return 0; }
};
static JavaVM vm;
struct JNIEnv { int GetJavaVM(JavaVM** out) { *out = &vm; return vm_error; } };
static void bootstrap(JNIEnv*) {
    ++bootstraps; free(dex_bytes); dex_bytes = nullptr; dex_size = 0;
}
static void stage_payload(const char*, JavaVM*) {
    ++stages; if (ticks < boot_at) ++violations;
}
'''

WRAPPER = r'''
extern "C" __declspec(dllexport) void run(int shell, int boot, int named,
        int attach_fail, int create_fail, int vm_fail, int has_dex) {
    ticks = attaches = detaches = bootstraps = stages = frees = violations = created = 0;
    pending = nullptr; started = 0;
    boot_at = boot; named_at = named;
    attach_error = attach_fail; create_error = create_fail; vm_error = vm_fail;
    dex_bytes = has_dex ? storage : nullptr; dex_size = has_dex ? sizeof storage : 0;
    JNIEnv env; attached_env = &env;
    if (shell) shell_loader(storage);
    else {
        started = vd_capability_start(&env);
        if (started) {
            if (attaches || bootstraps || ticks) ++violations; // Callback only dispatches.
            pending(pending_context);
        }
    }
}
extern "C" __declspec(dllexport) int value(int index) {
    int values[] = {ticks, attaches, detaches, bootstraps, stages, frees, violations, created, started};
    return values[index];
}
'''


@unittest.skipUnless(os.name == 'nt', 'freestanding Windows worker fixture')
class NativeBootWorkerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        parent = Path(tempfile.gettempdir()) / 'opencode'
        parent.mkdir(exist_ok=True)
        cls.temp = tempfile.TemporaryDirectory(prefix='vd-boot-workers-', dir=parent)
        cls.addClassCleanup(cls.temp.cleanup)
        out = Path(cls.temp.name)
        native = (ROOT / 'zygisk/jni/vd_capability.cpp').read_text(encoding='utf-8')
        worker = native[native.index('static bool system_server_started()'):]
        module = (ROOT / 'zygisk/jni/module.cpp').read_text(encoding='utf-8')
        shell = module[module.index('static void* shell_loader('):module.index('class VdHsModule')]
        (out / 'worker.cpp').write_text(MOCK + worker + shell + WRAPPER, encoding='utf-8')
        sdk = Path(os.environ['ANDROID_SDK_ROOT'])
        llvm = sdk / 'ndk/26.1.10909125/toolchains/llvm/prebuilt/windows-x86_64/bin'
        subprocess.run([str(llvm / 'clang++.exe'), '--target=x86_64-pc-windows-msvc',
                        '-ffreestanding', '-fno-stack-protector', '-fno-builtin', '-fno-exceptions',
                        '-fno-rtti', '-O2', '-Wall', '-Wextra', '-Werror', '-c', str(out / 'worker.cpp'),
                        '-o', str(out / 'worker.obj')], check=True)
        subprocess.run([str(llvm / 'ld.lld.exe'), '-flavor', 'link', '/dll', '/noentry',
                        '/nodefaultlib', '/machine:x64', '/out:' + str(out / 'worker.dll'),
                        str(out / 'worker.obj')], check=True)
        cls.lib = ctypes.CDLL(str(out / 'worker.dll'))
        cls.lib.run.argtypes = [ctypes.c_int] * 7
        cls.lib.run.restype = None
        cls.lib.value.argtypes = [ctypes.c_int]
        cls.lib.value.restype = ctypes.c_int
        kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        kernel.FreeLibrary.argtypes = [ctypes.c_void_p]
        kernel.FreeLibrary.restype = ctypes.c_int
        cls.addClassCleanup(lambda: kernel.FreeLibrary(cls.lib._handle))

    def run_worker(self, *, shell=0, boot=3, named=0, attach_fail=0,
                   create_fail=0, vm_fail=0, has_dex=1):
        self.lib.run(shell, boot, named, attach_fail, create_fail, vm_fail, has_dex)
        result = [self.lib.value(i) for i in range(9)]
        self.assertEqual(result[6], 0, 'work escaped the boot gate or ran on the specialize callback')
        return result

    def test_server_callback_only_dispatches_and_worker_attaches_after_boot(self):
        self.assertEqual(self.run_worker(), [3, 1, 1, 1, 0, 1, 0, 1, 1])

    def test_already_completed_boot_still_waits_for_post_fork_runtime(self):
        self.assertEqual(self.run_worker(boot=0, named=2), [2, 1, 1, 1, 0, 1, 0, 1, 1])

    def test_attach_failure_frees_capture_without_bootstrap_or_detach(self):
        self.assertEqual(self.run_worker(attach_fail=1), [3, 1, 0, 0, 0, 1, 0, 1, 1])

    def test_thread_creation_failure_frees_capture_without_running_worker(self):
        self.assertEqual(self.run_worker(create_fail=11), [0, 0, 0, 0, 0, 1, 0, 1, 0])

    def test_vm_failure_frees_capture_without_creating_thread(self):
        self.assertEqual(self.run_worker(vm_fail=-1), [0, 0, 0, 0, 0, 1, 0, 0, 0])

    def test_disabled_mode_or_missing_dex_does_not_dispatch(self):
        self.assertEqual(self.run_worker(has_dex=0), [0] * 9)

    def test_shell_staging_waits_for_boot_without_attaching_to_art(self):
        self.assertEqual(self.run_worker(shell=1), [3, 0, 0, 0, 1, 1, 0, 0, 0])


@unittest.skipUnless(SH, 'Git for Windows sh required')
class BootScriptTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        (Path(tempfile.gettempdir()) / 'opencode').mkdir(exist_ok=True)

    def run_coldstart(self, saved, module):
        with tempfile.TemporaryDirectory(prefix='vd-boot-request-', dir=Path(tempfile.gettempdir()) / 'opencode') as tmp:
            root = Path(tmp)
            (root / 'state').mkdir()
            if saved is not None:
                (root / 'state/requested').write_bytes(saved)
            if module is not None:
                (root / 'vd-capability.txt').write_bytes(module)
            source = (ROOT / 'zygisk/coldstart.sh').read_text(encoding='utf-8')
            source = source.replace('ROOT=/data/adb/vdhs-vd-capability', 'ROOT=./state')
            (root / 'coldstart.sh').write_text(source, encoding='utf-8', newline='\n')
            (root / 'run.sh').write_text('''PATH=/usr/bin:/bin
id() { printf 0; }
stat() { printf 0; }
flock() { printf 'lock\n' >> ./calls; }
chmod() { printf 'chmod\n' >> ./calls; }
sync() { printf 'sync\n' >> ./calls; }
set -- boot
. ./coldstart.sh
''', encoding='utf-8', newline='\n')
            result = subprocess.run([SH, './run.sh'], cwd=root, capture_output=True, timeout=15)
            path = root / 'vd-capability.txt'
            calls = (root / 'calls').read_text().splitlines() if (root / 'calls').exists() else []
            return result.returncode, path.read_bytes() if path.exists() else None, calls

    def test_unchanged_saved_choice_does_not_lock_write_chmod_or_sync(self):
        for value in (b'VDC2 0\n', b'VDC2 1\n'):
            with self.subTest(value=value):
                self.assertEqual(self.run_coldstart(value, value), (0, value, []))

    def test_missing_saved_choice_keeps_installed_configuration(self):
        self.assertEqual(self.run_coldstart(None, b'VDC2 0\n'), (0, b'VDC2 0\n', []))

    def test_explicit_boot_repair_retains_saved_choice_and_format(self):
        self.assertEqual(self.run_coldstart(b'VDC2 0\n', b'VDC2 1\n'),
                         (0, b'VDC2 0\n', ['chmod', 'lock', 'chmod', 'sync']))

    def test_base_gesture_mirror_waits_for_boot(self):
        # Base recovery can begin before boot; standalone also waits for its completion.
        source = (ROOT / 'zygisk/service.sh').read_text(encoding='utf-8')
        self.assertLess(source.index('mixedvr.sh" disable'), source.index('sys.boot_completed'))
        with tempfile.TemporaryDirectory(prefix='vd-service-boot-', dir=Path(tempfile.gettempdir()) / 'opencode') as tmp:
            root = Path(tmp)
            (root / 'payload').mkdir()
            (root / 'payload/mode.txt').write_bytes(b'2\n')
            source = source.replace('/data/adb/vdhs_zygisk.log', './service.log')
            (root / 'service.sh').write_text(source, encoding='utf-8', newline='\n')
            (root / 'run.sh').write_text('''PATH=/usr/bin:/bin
tick=0
getprop() { if [ "$tick" -ge 3 ]; then printf 1; else printf 0; fi; }
setprop() { printf '%s %s=%s\\n' "$tick" "$1" "$2" >> ./setprops; }
sleep() {
    tick=$((tick + 1))
    printf '%s\\n' "$tick" >> ./waits
    [ "$tick" -lt 5 ] || exit 0
}
. ./service.sh
wait
''', encoding='utf-8', newline='\n')
            result = subprocess.run([SH, './run.sh'], cwd=root, capture_output=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual((root / 'waits').read_text().splitlines()[:3], ['1', '2', '3'])
            self.assertEqual((root / 'setprops').read_text().splitlines()[0], '3 vdhs.system_gestures=0')


class BootIntegrationTests(unittest.TestCase):
    def test_post_fs_data_has_no_commands(self):
        lines = (ROOT / 'zygisk/post-fs-data.sh').read_text().splitlines()
        self.assertFalse([line for line in lines if line.strip() and not line.lstrip().startswith('#')])

    def test_saved_choice_precedes_module_choice_without_a_boot_rewrite(self):
        native = (ROOT / 'zygisk/jni/vd_capability.cpp').read_text()
        self.assertLess(native.index('/data/adb/vdhs-vd-capability/requested'),
                        native.index('small_file(directory, "vd-capability.txt"'))
        self.assertIn('if (!saved && !missing) return;', native)
        service = (ROOT / 'zygisk/service.sh').read_text()
        self.assertNotIn('coldstart.sh" boot', service)

    def test_non_targets_unload_and_only_shell_staging_is_deferred(self):
        source = (ROOT / 'zygisk/jni/module.cpp').read_text()
        skip = source[source.index('if (!match && !shell)'):source.index('int dir = api_->getModuleDir();')]
        self.assertIn('DLCLOSE_MODULE_LIBRARY', skip)
        post = source[source.index('void postAppSpecialize('):]
        self.assertNotIn('mkdir(', post)
        self.assertNotIn('dlopen(', post)
        self.assertLess(post.index('pthread_create('), post.index('stage_payload(dir, vm)'))
        self.assertIn('if (rc == 0) return;', post)

    def test_knuckles_dispatch_still_waits_before_enable_and_start(self):
        source = (ROOT / 'knuckles/headset/module/service.sh').read_text()
        self.assertLess(source.index('getprop sys.boot_completed'), source.index('standalone.sh" enable'))
        self.assertLess(source.index('getprop sys.boot_completed'), source.index('standalone.sh" start'))


if __name__ == '__main__':
    unittest.main(verbosity=2)
