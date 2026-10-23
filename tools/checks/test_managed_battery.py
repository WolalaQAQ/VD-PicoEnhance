import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
SDK = Path(os.environ.get("ANDROID_SDK_ROOT") or os.environ.get("ANDROID_HOME")
           or Path.home() / ".cache/pico-android/sdk")
BIN = SDK / "ndk/26.1.10909125/toolchains/llvm/prebuilt/windows-x86_64/bin"
BATTERY_APIS = (
    "mono_field_get_offset", "mono_field_get_type", "mono_field_get_flags",
    "mono_class_from_mono_type", "mono_class_is_valuetype", "mono_class_value_size",
    "mono_class_instance_size", "mono_type_get_type",
    "mono_threads_enter_gc_unsafe_region", "mono_threads_exit_gc_unsafe_region",
)


@unittest.skipUnless(os.name == "nt", "freestanding Windows Mono mock executable")
class ManagedBatteryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        parent = Path(tempfile.gettempdir()) / "opencode"
        parent.mkdir(exist_ok=True)
        cls.temp = tempfile.TemporaryDirectory(prefix="vd-managed-battery-", dir=parent)
        cls.addClassCleanup(cls.temp.cleanup)
        cls.directory = Path(cls.temp.name)
        # Execute the production Mono resolver, image fallback, layout checks,
        # and writer, excluding unrelated Android hand/mesh code and headers.
        source = (ROOT / "mod/native/vdhs_hand.c").read_text(encoding="utf-8")
        start = source.index("typedef void* MP;")
        end = source.index("static void log_exc(", start)
        (cls.directory / "managed_battery_source.h").write_text(
            source[start:end], encoding="utf-8")

    def run_case(self, case, missing_api=None):
        obj = self.directory / "battery.obj"
        exe = self.directory / "battery.exe"
        args = [str(BIN / "clang.exe"), "--target=x86_64-pc-windows-msvc",
                "-std=c11", "-ffreestanding", "-fno-stack-protector", "-fno-builtin",
                "-O2", "-Wall", "-Wextra", "-Werror", "-I", str(self.directory),
                f"-DBATTERY_CASE={case}"]
        if missing_api:
            args.append(f'-DMISSING_API="{missing_api}"')
        args += ["-c", str(Path(__file__).with_name("managed_battery_mock.c")), "-o", str(obj)]
        self.run_tool(args)
        self.run_tool([str(BIN / "ld.lld.exe"), "-flavor", "link", "/entry:entry",
                       "/subsystem:console", "/nodefaultlib", "/out:" + str(exe), str(obj)])
        result = subprocess.run([str(exe)], capture_output=True, timeout=15, check=False)
        self.assertEqual(result.returncode, 0,
                         f"case {case}, missing={missing_api}: mock assertion {result.returncode}")

    def run_tool(self, args):
        result = subprocess.run(args, capture_output=True, text=True, encoding="utf-8", timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_varied_offsets_only_change_two_bytes(self):
        self.run_case(0)

    def test_replaced_game_is_read_each_frame(self):
        self.run_case(1)

    def test_images_not_loaded_retry_without_writes(self):
        self.run_case(2)

    def test_missing_classes_and_fields_do_not_write(self):
        self.run_case(3)
        self.run_case(4)

    def test_wrong_field_types_and_flags_do_not_write(self):
        self.run_case(5)
        self.run_case(6)

    def test_invalid_offsets_sizes_and_overlap_do_not_write(self):
        self.run_case(7)
        self.run_case(8)
        self.run_case(9)

    def test_wrong_runtime_object_class_does_not_write(self):
        self.run_case(10)

    def test_domain_vtable_and_game_not_ready_retry(self):
        for case in (11, 12, 13):
            with self.subTest(case=case):
                self.run_case(case)

    def test_reference_and_value_type_layout_is_validated(self):
        self.run_case(14)

    def test_unknown_and_percentage_boundaries(self):
        self.run_case(15)

    def test_metadata_is_cached_but_game_is_not(self):
        self.run_case(16)

    def test_missing_apis_never_enter_gc_or_write(self):
        for api in (*BATTERY_APIS, "mono_image_loaded", "mono_object_get_class"):
            with self.subTest(api=api):
                self.run_case(17, missing_api=api)

    def test_image_loaded_name_miss_uses_assembly_fallback(self):
        self.run_case(18)

    def test_null_gc_cookie_is_still_balanced(self):
        self.run_case(19)


if __name__ == "__main__":
    unittest.main()
