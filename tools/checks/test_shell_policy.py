import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]


def find_shell():
    git = shutil.which("git")
    if git:
        shell = Path(git).resolve().parent.parent / "bin/sh.exe"
        if shell.is_file():
            return str(shell)
    for shell in (os.environ.get("BASH"), shutil.which("sh")):
        if shell and Path(shell).is_file():
            return shell
    return None


SH = find_shell()


@unittest.skipUnless(SH, "sh unavailable (Git for Windows, BASH, or PATH required)")
class ServiceMirrorTests(unittest.TestCase):
    def run_service(self, *, mode=b"2\n", setting=None, flags=(None,),
                    fail_attempts=(), change_module=False):
        parent = Path(tempfile.gettempdir()) / "opencode"
        parent.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="vd-service-", dir=parent) as tmp:
            root = Path(tmp)
            for directory in ("payload", "app", "flags", "mixedvr-state"):
                (root / directory).mkdir()
            (root / 'mixedvr-state/requested').write_text('0\n')
            for name, value in (("mode.txt", mode), ("system_gestures.txt", setting)):
                if value is not None:
                    (root / "payload" / name).write_bytes(value)
            for index, value in enumerate(flags):
                if value is not None:
                    (root / "flags" / str(index)).write_bytes(value)
            if flags[0] is not None:
                (root / "app/system_gestures.txt").write_bytes(flags[0])
            (root / "property").write_text("stale", encoding="utf-8")
            source = (ROOT / "zygisk/service.sh").read_text(encoding="utf-8")
            source = source.replace("/data/adb/vdhs_zygisk.log", "./service.log")
            source = source.replace('/data/adb/vdhs_mixedvr/', './mixedvr-state/')
            source = source.replace("/data/data/VirtualDesktop.Android/vdhs/system_gestures.txt",
                                    "./app/system_gestures.txt")
            (root / "service.sh").write_bytes(source.encode("utf-8"))
            failure = ""
            if fail_attempts:
                cases = "|".join(str(attempt) for attempt in fail_attempts)
                failure = f'case "$attempt" in {cases}) return 1 ;; esac\n'
            module_change = ""
            if change_module:
                module_change = ("printf '1\\n' > ./payload/mode.txt\n"
                                 "printf '0\\n' > ./payload/system_gestures.txt\n")
            runner = f"""attempt=0
tick=0
date() {{ printf 'test-date'; }}
getprop() {{ printf '1'; }}
setprop() {{
    attempt=$((attempt + 1))
    printf '%s=%s\\n' "$1" "$2" >> ./attempts.log
    {failure}printf '%s' "$2" > ./property
}}
sleep() {{
    printf '%s\\n' "$1" >> ./sleeps.log
    tick=$((tick + 1))
    [ "$tick" -lt {len(flags)} ] || exit 0
    {module_change}if [ -f "./flags/$tick" ]; then
        cp "./flags/$tick" ./app/system_gestures.txt
    else
        rm -f ./app/system_gestures.txt
    fi
}}
. ./service.sh
wait
"""
            (root / "runner.sh").write_bytes(runner.encode("utf-8"))
            result = subprocess.run([SH, "./runner.sh"], cwd=root, capture_output=True,
                                    text=True, timeout=15, check=False)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stderr, "")
            attempts = root / "attempts.log"
            writes = attempts.read_text().splitlines() if attempts.exists() else []
            for write in writes:
                self.assertTrue(write.startswith("vdhs.system_gestures="), write)
            sleeps = root / "sleeps.log"
            intervals = sleeps.read_text().splitlines() if sleeps.exists() else []
            return ([write.split("=", 1)[1] for write in writes], intervals,
                    (root / "property").read_text())

    def assert_mirrors(self, expected, **kwargs):
        writes, intervals, value = self.run_service(**kwargs)
        self.assertEqual(writes, expected)
        self.assertEqual(intervals, ["1"] * len(kwargs.get("flags", (None,))))
        self.assertEqual(value, expected[-1])

    def test_default_is_off_without_external_opt_in(self):
        self.assert_mirrors(["0"], mode=(ROOT / "zygisk/payload/mode.txt").read_bytes())

    def test_non_full_modes_have_no_mirror(self):
        for mode in (b"0\n2\n", b"1\n2\n", b" # comment\r\n\t1 \r# mode\r\n2"):
            with self.subTest(mode=mode):
                self.assertEqual(self.run_service(mode=mode), ([], [], "stale"))

    def test_legacy_support_settings_do_not_gate_the_ui(self):
        for setting in (None, b"0\n1\n", b"2\n1\n", b"", b" \r\n# comment\n"):
            with self.subTest(setting=setting):
                self.assert_mirrors(["1"], setting=setting, flags=(b"1",))

    def test_invalid_legacy_support_settings_are_ignored(self):
        for setting in (b"enabled\n1\n", b"10\n1\n", b"1 1\n1\n", b"1x\n1\n",
                        b"1\v\n1\n", b"1\f\n1\n"):
            with self.subTest(setting=setting):
                self.assert_mirrors(["1"], setting=setting, flags=(b"1",))

    def test_missing_mode_defaults_to_full(self):
        self.assert_mirrors(["1"], mode=None, flags=(b"1",))

    def test_invalid_mode_uses_default_not_later_value(self):
        for mode in (b"", b" \r\n# comment\n", b"invalid\n1\n", b"21\n1\n", b"2 1\n1\n"):
            with self.subTest(mode=mode):
                self.assert_mirrors(["1"], mode=mode, flags=(b"1",))

    def test_module_config_comments_crlf_and_inline_comments(self):
        self.assert_mirrors(["1"],
                            mode=b"#" + b"long comment " * 1000 + b"\r\n \t\r\n\t2 \t\r# full\r\n1",
                            setting=b"\r\n\t # support\r\n \t1\t\r # enabled\r\n0",
                            flags=(b"1",))
        self.assert_mirrors(["1"], mode=b"2#full", setting=b"1#enabled", flags=(b"1",))

    def test_on_off_round_trip(self):
        self.assert_mirrors(["1", "0", "1"], flags=(b"1", b"0", b"1"))

    def test_unchanged_normalized_value_is_written_once(self):
        self.assert_mirrors(["1"], flags=(b"1", b"1\r\n", b"\r1\n\r\n", b"1"))

    def test_first_missing_or_invalid_flag_restores_gestures(self):
        for flag in (None, b"", b"invalid", b"2", b" 1", b"1 ", b"\t1", b"1#enabled",
                     b"0\n1"):
            with self.subTest(flag=flag):
                self.assert_mirrors(["0"], flags=(flag, None, b"invalid"))

    def test_invalid_and_missing_flags_switch_off_only_on_change(self):
        self.assert_mirrors(["1", "0"],
                            flags=(b"1", b"invalid", None, b"", b" 0", b"0\r\n", b"0"))

    def test_failed_first_write_is_retried(self):
        self.assert_mirrors(["1", "1"], flags=(b"1", b"1", b"1"), fail_attempts=(1,))

    def test_failed_default_off_is_retried(self):
        self.assert_mirrors(["0", "0"], flags=(None, None, None), fail_attempts=(1,))

    def test_failed_transition_is_retried(self):
        self.assert_mirrors(["1", "0", "0"], flags=(b"1", b"0", b"0", b"0"),
                            fail_attempts=(2,))

    def test_module_settings_are_not_hot_switches(self):
        self.assert_mirrors(["1", "0", "1"], flags=(b"1", b"0", b"1"), change_module=True)


class ShellPolicyTests(unittest.TestCase):
    def test_disabled_by_default(self):
        self.assertFalse((ROOT / "zygisk/payload/system_gestures.txt").exists())
        source = (ROOT / "mod/src/VdHsMod/GestureToggle.cs").read_text()
        self.assertIn("bool initial = false;", source)

    def test_gate_precedes_staging(self):
        source = (ROOT / "zygisk/jni/module.cpp").read_text()
        gate = source.index("if (shell && !vdhs_shell_requested(")
        stage = source.index("g_active = shell ?")
        self.assertLess(gate, stage)
        self.assertIn("close(dir);", source[gate:stage])
        self.assertIn("api_->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);", source[gate:stage])
        self.assertNotIn("com.pico.xr.openxr_runtime", source)
        self.assertIn("vdhs_config_read(stream, fallback)", source)
        payload = (ROOT / "mod/native/vdhs_payload.c").read_text()
        self.assertIn("vdhs_config_read(f, fallback)", payload)
        self.assertIn("return value > 0 ? value : fallback;", payload)

    def test_payload_checks_full_mode(self):
        source = (ROOT / "mod/native/vdhs_shell.c").read_text()
        entry = source[source.index("int vdhs_shell_main("):]
        self.assertLess(entry.index("if (mode != 2) return 0;"), entry.index("shell_thread(NULL)"))
        self.assertNotIn("pthread_create(", entry)
        self.assertIn("g_original(self, package, state, is_self);", source)

    @unittest.skipUnless(os.name == "nt", "freestanding Windows policy executable")
    def test_native_policy(self):
        sdk = Path(os.environ.get("ANDROID_SDK_ROOT", Path.home() / ".cache/pico-android/sdk"))
        llvm = sdk / "ndk/26.1.10909125/toolchains/llvm/prebuilt/windows-x86_64/bin"
        clang = llvm / "clang.exe"
        linker = llvm / "ld.lld.exe"
        self.assertTrue(clang.is_file(), str(clang))
        self.assertTrue(linker.is_file(), str(linker))
        parent = Path(os.environ["TEMP"]) / "opencode"
        parent.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="vd-shell-policy-", dir=parent) as tmp:
            (Path(tmp) / "stdio.h").write_text(
                "typedef struct { const char* next; } FILE;\n#define EOF (-1)\nint fgetc(FILE*);\n")
            (Path(tmp) / "string.h").write_text(
                "#include <stddef.h>\nsize_t strlen(const char*);\n"
                "void* memcpy(void*, const void*, size_t);\nint memcmp(const void*, const void*, size_t);\n")
            obj = Path(tmp) / "policy.obj"
            exe = Path(tmp) / "policy.exe"
            subprocess.run([str(clang), "--target=x86_64-pc-windows-msvc", "-ffreestanding",
                            "-fno-stack-protector", "-fno-builtin", "-O2", "-Wall", "-Wextra", "-Werror", "-c",
                            "-I", tmp, str(ROOT / "tools/checks/test-shell-policy.c"), "-o", str(obj)], check=True)
            subprocess.run([str(linker), "-flavor", "link", "/entry:entry", "/subsystem:console", "/nodefaultlib",
                            "/out:" + str(exe), str(obj)], check=True)
            result = subprocess.run([str(exe)], check=False)
            self.assertEqual(result.returncode, 0, "policy assertion " + str(result.returncode))


if __name__ == "__main__":
    unittest.main()
