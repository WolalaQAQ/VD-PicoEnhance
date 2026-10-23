"""Offline OpenXR lifecycle tests executing the production battery C module."""

import ctypes
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
SDK = Path(os.environ.get("ANDROID_SDK_ROOT", Path.home() / ".cache/pico-android/sdk"))
BIN = SDK / "ndk/26.1.10909125/toolchains/llvm/prebuilt/windows-x86_64/bin"


@unittest.skipUnless(os.name == "nt", "Windows host C harness")
class ControllerBatteryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(
            prefix="vdhs-battery-", dir=Path(os.environ["TEMP"]) / "opencode")
        cls.addClassCleanup(cls.temp.cleanup)
        temp = Path(cls.temp.name)
        (temp / "android").mkdir()
        headers = {
            "android/log.h": "#define ANDROID_LOG_INFO 4\n#define ANDROID_LOG_ERROR 6\n"
                             "int __android_log_print(int, const char*, const char*, ...);\n",
            "string.h": "#include <stddef.h>\nvoid* memcpy(void*, const void*, size_t);\n"
                        "void* memset(void*, int, size_t);\nint strcmp(const char*, const char*);\n",
            "time.h": "struct timespec { long long tv_sec, tv_nsec; };\n"
                      "#define CLOCK_MONOTONIC 1\nint clock_gettime(int, struct timespec*);\n",
            "shadowhook.h": "void* shadowhook_hook_func_addr(void*, void*, void**);\n"
                             "int shadowhook_unhook(void*);\nint shadowhook_get_errno(void);\n"
                             "const char* shadowhook_to_errmsg(int);\n",
        }
        for name, text in headers.items():
            (temp / name).write_text(text, encoding="utf-8")
        layer = (ROOT / "mod/native/vdhs_layer.c").read_text(encoding="utf-8")

        def between(start, end):
            first = layer.index(start)
            return layer[first:layer.index(end, first)]

        # Fail at a missing boundary rather than silently compiling a stale copy.
        declarations = between("typedef int32_t XrResult;", "// vdhs_payload.c")
        declarations += between("static const char kLoaderNeedle[]", "static int64_t now_ms(")
        declarations += between("// Extensions the layer serves itself:",
                                "// ---------------------------------------------------------------- passthrough diagnostics")
        functions = between("int vdhs_layer_active(void)", "int vdhs_layer_started(void)")
        functions += between("static int is_layer_ext(", "static XrResult hk_gipa(")
        functions += between("static XrResult hk_create_action(", "static int is_grip(")
        functions += between("typedef struct { const char* name; void* hook;", "static void* layer_thread(")
        (temp / "layer_battery_declarations.h").write_text(declarations, encoding="utf-8")
        (temp / "layer_battery_functions.h").write_text(functions, encoding="utf-8")

        def run(command):
            result = subprocess.run([str(arg) for arg in command], capture_output=True,
                                     text=True, encoding="utf-8", cwd=ROOT, timeout=60)
            if result.returncode:
                raise AssertionError(result.stdout + result.stderr)

        def build(name, integration):
            objects = []
            sources = ["battery_mock.c"] + (["battery_layer_mock.c"] if integration else [])
            for source in sources:
                obj = temp / (name + "-" + source + ".obj")
                command = [BIN / "clang.exe", "--target=x86_64-pc-windows-msvc", "-std=c11", "-O2",
                           "-Wall", "-Wextra", "-Werror", "-ffreestanding", "-fno-builtin",
                           "-fno-stack-protector", "-mno-stack-arg-probe", "-I", temp]
                if integration:
                    command.append("-DBATTERY_LAYER_INTEGRATION")
                run(command + ["-c", Path(__file__).with_name(source), "-o", obj])
                objects.append(obj)
            dll = temp / (name + ".dll")
            run([BIN / "ld.lld.exe", "-flavor", "link", "/dll", "/noentry", "/nodefaultlib",
                 "/machine:x64", "/out:" + str(dll), *objects])
            return ctypes.CDLL(str(dll))

        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.FreeLibrary.argtypes = [ctypes.c_void_p]
        kernel.FreeLibrary.restype = ctypes.c_int
        cls.lib = build("battery", False)
        cls.addClassCleanup(lambda: kernel.FreeLibrary(cls.lib._handle))
        cls.integration = build("battery-layer", True)
        cls.addClassCleanup(lambda: kernel.FreeLibrary(cls.integration._handle))
        cls.lib.battery_case.argtypes = [ctypes.c_int]
        cls.lib.battery_case.restype = ctypes.c_int
        cls.integration.battery_integration_case.argtypes = [ctypes.c_int]
        cls.integration.battery_integration_case.restype = ctypes.c_int

    def check_case(self, scenario):
        self.assertEqual(self.lib.battery_case(scenario), 0, "battery_mock.c assertion line")

    def check_integration(self, scenario):
        self.assertEqual(self.integration.battery_integration_case(scenario), 0,
                         "battery_mock.c integration assertion line")

    def test_existing_bindings_and_left_right_values(self): self.check_case(0)
    def test_other_profiles_unchanged(self): self.check_case(1)
    def test_rejected_battery_keeps_original_input(self): self.check_case(2)
    def test_inactive_layer_is_transparent(self): self.check_case(3)
    def test_partial_hook_install_stays_inactive(self): self.check_case(4)
    def test_inactive_and_failed_samples_are_unknown(self): self.check_case(5)
    def test_out_of_range_samples_are_unknown(self): self.check_case(6)
    def test_sampling_once_per_second(self): self.check_case(7)
    def test_new_session_samples_immediately(self): self.check_case(8)
    def test_failed_sync_does_not_send_stale_charge(self): self.check_case(9)
    def test_fields_refreshed_without_repolling(self): self.check_case(10)
    def test_new_instance_resets_action_and_cache(self): self.check_case(11)
    def test_float_action_in_existing_set_created_once(self): self.check_case(12)
    def test_action_failure_does_not_change_bindings(self): self.check_case(13)
    def test_path_failure_does_not_create_action(self): self.check_case(14)
    def test_destroyed_instance_handles_are_rejected(self): self.check_case(15)
    def test_foreign_instance_session_and_action_handles_are_rejected(self): self.check_case(16)
    def test_reads_require_attachment_and_sync_of_the_controller_set(self): self.check_case(17)
    def test_actions_cannot_be_created_after_attachment(self): self.check_case(18)
    def test_byte_outlier_and_nan_are_unknown(self): self.check_case(19)
    def test_zero_is_valid_empty_battery(self): self.check_case(20)
    def test_both_controllers_inactive_from_first_sync_preserves_original_calls(self): self.check_case(21)
    def test_waiting_managed_writer_cannot_change_original_sync(self): self.check_case(22)
    def test_unavailable_managed_writer_cannot_change_original_sync(self): self.check_case(23)
    def test_no_grip_action_keeps_original_binding_and_sync(self): self.check_case(24)

    def test_real_layer_install_create_bind_attach_sync_and_read(self): self.check_integration(0)
    def test_real_layer_failed_instance_does_not_initialize_battery(self): self.check_integration(1)
    def test_real_layer_failed_grip_does_not_create_battery_action(self): self.check_integration(2)
    def test_real_layer_optional_hook_failure_preserves_original_input(self): self.check_integration(3)
    def test_real_layer_new_instance_recreates_and_reads_new_action(self): self.check_integration(4)
    def test_real_layer_cold_start_without_active_pico_controllers(self): self.check_integration(5)
    def test_real_layer_without_controllers_or_ready_writer_stays_active(self): self.check_integration(6)
    def test_real_layer_unavailable_battery_writer_stays_active(self): self.check_integration(7)


if __name__ == "__main__":
    unittest.main()
