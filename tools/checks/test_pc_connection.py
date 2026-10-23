"""Execute the native VD startup connection bootstrap with owned Win32 mocks."""
import ctypes
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SDK = Path(os.environ.get('ANDROID_SDK_ROOT', Path.home()/'.cache/pico-android/sdk'))
BIN = SDK/'ndk/26.1.10909125/toolchains/llvm/prebuilt/windows-x86_64/bin'


@unittest.skipUnless(os.name == 'nt', 'Windows native-code harness')
class ConnectionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='vd-connection-', dir=Path(os.environ['TEMP'])/'opencode')
        cls.addClassCleanup(cls.temp.cleanup)
        temp = Path(cls.temp.name)
        sources = ROOT/'pc/driver/src'
        objects = []
        for source in (sources/'connection.c', sources/'memory.c', Path(__file__).with_name('connection_mock.c')):
            obj = temp/(source.name+'.obj')
            command = [BIN/'clang.exe', '--target=x86_64-pc-windows-msvc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-ffreestanding', '-fno-builtin', '-fno-stack-protector', '-mno-stack-arg-probe', '-I', sources, '-c', source, '-o', obj]
            result = subprocess.run(list(map(str, command)), capture_output=True, text=True, timeout=30)
            if result.returncode: raise AssertionError(result.stdout+result.stderr)
            objects.append(obj)
        dll = temp/'connection.dll'
        result = subprocess.run(list(map(str, [BIN/'ld.lld.exe', '-flavor', 'link', '/dll', '/noentry', '/nodefaultlib', '/machine:x64', '/out:'+str(dll), *objects])), capture_output=True, text=True, timeout=30)
        if result.returncode: raise AssertionError(result.stdout+result.stderr)
        cls.lib = ctypes.CDLL(str(dll))
        kernel = ctypes.WinDLL('kernel32')
        kernel.FreeLibrary.argtypes = [ctypes.c_void_p]
        cls.addClassCleanup(lambda: kernel.FreeLibrary(cls.lib._handle))
        cls.lib.connection_case.argtypes = [ctypes.c_int]

    def check(self, scenario): self.assertEqual(self.lib.connection_case(scenario), 0, 'connection_mock.c assertion line')
    def test_legacy_missing_configuration_uses_existing_forward(self): self.check(0)
    def test_unreadable_configuration_does_not_launch(self): self.check(1)
    def test_usb_forward_and_unicode_executable_path(self): self.check(2)
    def test_wireless_connect_then_forward_without_shell(self): self.check(3)
    def test_create_process_failure(self): self.check(4)
    def test_missing_optional_process_api(self): self.check(5)
    def test_process_timeout_terminates_only_owned_child(self): self.check(6)
    def test_stop_cancels_pending_child(self): self.check(7)
    def test_non_utf16_configuration(self): self.check(8)
    def test_missing_serial(self): self.check(9)
    def test_quoted_executable_path_rejected(self): self.check(10)
    def test_serial_cannot_add_arguments(self): self.check(11)
    def test_wait_failure_closes_handles(self): self.check(12)
    def test_read_failure_closes_file(self): self.check(13)
    def test_cleanup_is_idempotent(self): self.check(14)


if __name__ == '__main__': unittest.main(verbosity=2)
