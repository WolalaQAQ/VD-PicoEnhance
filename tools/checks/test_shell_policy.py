import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]


class ShellPolicyTests(unittest.TestCase):
    def test_disabled_by_default(self):
        setting = ROOT / "zygisk/payload/system_gestures.txt"
        values = [line.strip() for line in setting.read_text().splitlines()
                  if line.strip() and not line.lstrip().startswith("#")]
        self.assertEqual(values, ["0"])

    def test_gate_precedes_staging(self):
        source = (ROOT / "zygisk/jni/module.cpp").read_text()
        gate = source.index("if (shell && !vdhs_shell_requested(")
        stage = source.index("g_active = shell ?")
        self.assertLess(gate, stage)
        self.assertIn("close(dir);\n            return;", source[gate:stage])
        self.assertNotIn("com.pico.xr.openxr_runtime", source)
        self.assertIn("vdhs_config_read(stream, fallback)", source)
        payload = (ROOT / "mod/native/vdhs_payload.c").read_text()
        self.assertIn("vdhs_config_read(f, fallback)", payload)
        self.assertIn("return value > 0 ? value : fallback;", payload)

    def test_payload_checks_full_mode(self):
        source = (ROOT / "mod/native/vdhs_shell.c").read_text()
        entry = source[source.index("int vdhs_shell_main("):]
        self.assertLess(entry.index("if (mode != 2) return 0;"), entry.index("pthread_create("))
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
