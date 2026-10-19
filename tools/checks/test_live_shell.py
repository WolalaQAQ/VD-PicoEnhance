"""Execute the real Shell hook body with mocked properties and package inputs."""
import ctypes
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
MOCK = r'''
#include <stddef.h>
#include "vdhs_shell_abi.h"
#include "vdhs_shell_policy.h"
#define PROP_VALUE_MAX 92
#define LOGI(...) ((void)0)
static const char* kVd = "VirtualDesktop.Android";
static const char* kSwitchProperty = "vdhs.system_gestures";
static unsigned char storage;
static unsigned char* g_disabled = &storage;
static int property_value, original_calls;
size_t strlen(const char* s) { size_t n = 0; while (s[n]) ++n; return n; }
void* memcpy(void* d, const void* s, size_t n) {
    unsigned char* a = d; const unsigned char* b = s;
    for (size_t i = 0; i < n; ++i) a[i] = b[i]; return d;
}
int memcmp(const void* a, const void* b, size_t n) {
    const unsigned char* x = a; const unsigned char* y = b;
    for (size_t i = 0; i < n; ++i) if (x[i] != y[i]) return x[i] - y[i]; return 0;
}
static int __system_property_get(const char* name, char* value) {
    (void)name; if (property_value < 0) return 0;
    if (property_value == 10) { value[0] = '1'; value[1] = '0'; value[2] = 0; return 2; }
    value[0] = (char)('0' + property_value); value[1] = 0; return 1;
}
static void g_original(void* self, const char* pkg, const void* state, int is_self) {
    (void)self; (void)pkg; (void)state; (void)is_self; ++original_calls;
}
'''
WRAPPER = r'''
__declspec(dllexport) int evaluate(int value, int state, int is_self, int null_state, int other) {
    const char* name = other ? "com.example.other" : kVd;
    char package[24] = {0}; package[0] = (char)(strlen(name) << 1);
    memcpy(package + 1, name, strlen(name));
    // Start suppressed, so missing/invalid/OFF must actively restore the flag.
    property_value = value; storage = 1; original_calls = 0;
    hook_update(0, package, null_state ? 0 : &state, is_self);
    return storage | (original_calls << 8);
}
'''


@unittest.skipUnless(os.name == 'nt', 'freestanding Windows hook mock')
class LiveShellTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        parent = Path(tempfile.gettempdir()) / 'opencode'
        parent.mkdir(exist_ok=True)
        cls.temp = tempfile.TemporaryDirectory(prefix='vd-live-shell-', dir=parent)
        cls.addClassCleanup(cls.temp.cleanup)
        output = Path(cls.temp.name)
        text = (ROOT / 'mod/native/vdhs_shell.c').read_text(encoding='utf-8')
        hook = text[text.index('static void hook_update('):text.index('static void* shell_thread(')]
        (output / 'string.h').write_text(
            '#include <stddef.h>\nsize_t strlen(const char*);\n'
            'void* memcpy(void*, const void*, size_t);\n'
            'int memcmp(const void*, const void*, size_t);\n')
        (output / 'hook.c').write_text(MOCK + hook + WRAPPER, encoding='utf-8')
        sdk = Path(os.environ.get('ANDROID_SDK_ROOT', Path.home() / '.cache/pico-android/sdk'))
        bin_dir = sdk / 'ndk/26.1.10909125/toolchains/llvm/prebuilt/windows-x86_64/bin'
        subprocess.run([str(bin_dir / 'clang.exe'), '--target=x86_64-pc-windows-msvc',
                        '-ffreestanding', '-fno-stack-protector', '-fno-builtin', '-O2',
                        '-Wall', '-Wextra', '-Werror', '-I', str(output),
                        '-I', str(ROOT / 'mod/native'), '-c', str(output / 'hook.c'),
                        '-o', str(output / 'hook.obj')], check=True)
        dll = output / 'hook.dll'
        subprocess.run([str(bin_dir / 'ld.lld.exe'), '-flavor', 'link', '/dll', '/noentry',
                        '/nodefaultlib', '/machine:x64', '/out:' + str(dll),
                        str(output / 'hook.obj')], check=True)
        cls.lib = ctypes.CDLL(str(dll))
        cls.lib.evaluate.argtypes = [ctypes.c_int] * 5
        cls.lib.evaluate.restype = ctypes.c_int
        kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        kernel.FreeLibrary.argtypes = [ctypes.c_void_p]
        kernel.FreeLibrary.restype = ctypes.c_int
        cls.addClassCleanup(lambda: kernel.FreeLibrary(cls.lib._handle))

    def test_live_switch_preserves_focus_predicate(self):
        for prop in [-1, 0, 1, 2, 10]:
            for state, is_self, null_state in [(6, 0, 0), (0, 0, 0), (6, 1, 0), (6, 0, 1)]:
                with self.subTest(property=prop, state=state, is_self=is_self, null_state=null_state):
                    expected = int(prop == 1 and state == 6 and not is_self and not null_state)
                    self.assertEqual(self.lib.evaluate(prop, state, is_self, null_state, 0), expected)

    def test_other_packages_keep_original_path(self):
        for prop in [-1, 0, 1]:
            self.assertEqual(self.lib.evaluate(prop, 6, 0, 0, 1), 257)


if __name__ == '__main__':
    unittest.main()
