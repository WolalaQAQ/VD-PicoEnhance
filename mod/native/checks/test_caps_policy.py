"""Exercise the actual pure caps policy in a freestanding offline executable.

Run: python -B -X utf8 -m unittest discover -s mod/native/checks -p '*caps*.py' -v
Uses the pinned NDK clang/lld. No CRT, Zig cache, VR runtime or device access.
"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


NATIVE = Path(__file__).resolve().parents[1]
SDK = Path(os.environ.get("ANDROID_SDK_ROOT") or os.environ.get("ANDROID_HOME")
           or Path.home() / ".cache/pico-android/sdk")
CLANG = SDK / "ndk/26.1.10909125/toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe"
FLAGS = ["--target=aarch64-linux-android29", "-std=c11", "-Wall", "-Wextra",
         "-Werror", "-Wno-unused-parameter"]

EYE = "XR_EXT_eye_gaze_interaction"
# VD 1.34.22 XR._picoExtensions, not a hypothetical FB/ANDROID face instance.
PICO_EXTENSIONS = [
    "XR_KHR_android_create_instance", "XR_KHR_convert_timespec_time",
    "XR_KHR_composition_layer_color_scale_bias", "XR_KHR_composition_layer_cube",
    "XR_KHR_composition_layer_cylinder", "XR_KHR_composition_layer_equirect",
    "XR_KHR_composition_layer_equirect2", "XR_KHR_opengl_es_enable",
    "XR_EXT_performance_settings", EYE, "XR_FB_color_space",
    "XR_FB_composition_layer_image_layout", "XR_FB_composition_layer_settings",
    "XR_FB_display_refresh_rate", "XR_FB_passthrough", "XR_FB_triangle_mesh", "XR_BD_body_tracking",
]

# Expected packed state: eye | (PICO hardware hint << 2) | (legacy face alias << 4).
# unknown=0, unsupported=1, supported=2. Expectations are explicit, not a Python
# reimplementation of the C reducer. Allowing the legacy permission alias does
# not change UNKNOWN into a supported face-capability claim.
CASES = [
    ("unknown_retains_legacy_not_support", "", [], [], 0, 0, 16),
    ("extension_is_not_hardware", "", [EYE], [], 0, 0, 16),
    ("p4p_real_pico_instance", "1", PICO_EXTENSIONS,
     [(1000030000, 1), (1000201004, 0), (1000458003, 0)], 0, 0, 26),
    ("ordinary_pico_no_hardware", "0", PICO_EXTENSIONS, [(1000030000, 0)], 0, 0, 5),
    ("runtime_eye_does_not_imply_face_hardware", "0", [EYE], [(1000030000, 1)], 0, 0, 6),
    ("gaze_no_does_not_remove_face_hardware_hint", "1", [EYE], [(1000030000, 0)], 0, 0, 25),
    ("eye_yes_does_not_promote_unknown_hint", "", [EYE], [(1000030000, 1)], 0, 0, 18),
    ("old_wrong_eye_type", "1", [EYE], [(1000030001, 1)], 0, 0, 24),
    ("unrequested_eye_not_evidence", "1", [], [(1000030000, 1)], 0, 0, 24),
    ("unused_face_zeros_not_hardware_evidence", "", [EYE],
     [(1000201004, 0), (1000458003, 0)], 0, 0, 16),
    ("unused_face_true_not_hardware_evidence", "0", [EYE], [(1000201004, 1)], 0, 0, 4),
    ("hand_property_independent", "1", [EYE], [(1000051000, 1)], 0, 0, 24),
    ("failed_query_retains_eye_support", "1", [EYE], [(1000030000, 0)], 2, -1, 26),
    ("failed_query_cannot_create_eye_support", "1", [EYE], [(1000030000, 1)], 0, -1, 24),
    ("failed_query_keeps_unknown_hardware", "", [EYE], [(1000030000, 0)], 0, -1, 16),
    ("missing_chain_retains_eye_support", "1", [EYE], [], 2, 0, 26),
    ("explicit_gaze_no_overrides_previous_yes", "1", [EYE], [(1000030000, 0)], 2, 0, 25),
    ("unrelated_extensions", "1", ["XR_EXT_hand_tracking", "XR_FB_passthrough", EYE, EYE],
     [(1000030000, 1)], 0, 0, 26),
]


def policy_probe_source():
    lines = ['#include "vdhs_caps.c"', '#include <stddef.h>',
             '_Static_assert(offsetof(vdhs_caps_property, supported) == 16, "XrBool32 ABI");']
    expected = {}
    for name, hint, extensions, properties, previous, result, value in CASES:
        lines.append(f"int caps_{name}(void) {{")
        lines.append(f'vdhs_cap hint = vdhs_caps_pico_etft_hint("{hint}");')
        if extensions:
            names = ", ".join(f'"{ext}"' for ext in extensions)
            lines.append(f"const char* names[] = {{ {names} }};")
            lines.append(f"int enabled = vdhs_caps_has_eye_gaze_ext({len(extensions)}, names);")
        else:
            lines.append("int enabled = vdhs_caps_has_eye_gaze_ext(0, 0);")
        for i in reversed(range(len(properties))):
            prop = properties[i]
            next_pointer = f"&p{i + 1}" if i + 1 < len(properties) else "0"
            values = ", ".join(str(n) for n in prop[1:])
            lines.append(f"vdhs_caps_property p{i} = {{ {prop[0]}u, {next_pointer}, {values} }};")
        next_pointer = "&p0" if properties else "0"
        lines.append(f"vdhs_cap eye = vdhs_caps_update_eye_support({previous}, {result}, enabled, {next_pointer});")
        lines.append("return eye | (hint << 2) | (vdhs_caps_face_permission_policy(hint) << 4); }")
        expected[f"caps_{name}"] = value
    for i, (text, value) in enumerate([("", 0), ("0", 1), ("1", 2), ("true", 0), ("2", 0), ("1 ", 0)]):
        lines.append(f'int caps_hint_{i}(void) {{ return vdhs_caps_pico_etft_hint("{text}"); }}')
        expected[f"caps_hint_{i}"] = value
    for hint in range(3):
        for runtime in range(3):
            name = f"caps_capture_{hint}_{runtime}"
            lines.append(f"int {name}(void) {{ return vdhs_caps_capture_eye_policy({hint}, {runtime}); }}")
            expected[name] = int(runtime == 2 or (runtime == 0 and hint != 1))
    names = ", ".join(f'"{ext}"' for ext in PICO_EXTENSIONS)
    lines.append(f"static const char* pico_extensions[] = {{ {names} }};")
    # Exercise production initialization/query/getter plumbing too, with only
    # pthread/property/log imports replaced by deterministic single-thread stubs.
    lines.append(r'''
static char test_property = '1';
int __system_property_get(const char* name, char* value) {
    if (strcmp(name, "ro.pxr.eyetracking.support")) return 0;
    value[0] = test_property; value[1] = 0;
    return test_property != 0;
}
int pthread_once(pthread_once_t* once, void (*init)(void)) { if (!*once) { init(); *once = 1; } return 0; }
int pthread_mutex_lock(pthread_mutex_t* mutex) { (void)mutex; return 0; }
int pthread_mutex_unlock(pthread_mutex_t* mutex) { (void)mutex; return 0; }
int __android_log_print(int level, const char* tag, const char* fmt, ...) {
    (void)level; (void)tag; (void)fmt; return 0;
}
int caps_production_pico_sequence(void) {
    vdhs_caps_property no = { 1000030000u, 0, 0 };
    vdhs_caps_property unused_face = { 1000201004u, 0, 0 };
    vdhs_caps_property yes = { 1000030000u, &unused_face, 1 };
    vdhs_caps_begin_instance(17, pico_extensions);
    if (g_pico_etft_hint != VDHS_CAP_SUPPORTED || !vdhs_caps_fix_face_permission()) return 1;
    if (vdhs_caps_eye_supported() || !vdhs_caps_capture_eye_settings()) return 2;
    vdhs_caps_observe_system_properties(0, &yes);
    if (!vdhs_caps_eye_supported() || !vdhs_caps_fix_face_permission()) return 3;
    vdhs_caps_observe_system_properties(-1, &no);
    vdhs_caps_observe_system_properties(0, 0);
    if (!vdhs_caps_eye_supported() || !vdhs_caps_fix_face_permission()) return 4;

    test_property = '0'; g_hint_once = 0;
    vdhs_caps_begin_instance(17, pico_extensions);
    vdhs_caps_observe_system_properties(0, &no);
    if (vdhs_caps_eye_supported() || vdhs_caps_fix_face_permission() || vdhs_caps_capture_eye_settings()) return 5;
    vdhs_caps_observe_system_properties(0, &yes);
    if (!vdhs_caps_eye_supported() || vdhs_caps_fix_face_permission()) return 6;

    test_property = 0; g_hint_once = 0;
    vdhs_caps_begin_instance(17, pico_extensions);
    if (g_pico_etft_hint != VDHS_CAP_UNKNOWN || !vdhs_caps_fix_face_permission()) return 7;
    if (vdhs_caps_eye_supported() || !vdhs_caps_capture_eye_settings()) return 8;
    vdhs_caps_observe_system_properties(0, &yes);
    if (g_pico_etft_hint != VDHS_CAP_UNKNOWN || !vdhs_caps_fix_face_permission()) return 9;

    test_property = '1'; g_hint_once = 0;
    vdhs_caps_begin_instance(17, pico_extensions);
    vdhs_caps_observe_system_properties(0, &no);
    if (vdhs_caps_eye_supported() || !vdhs_caps_fix_face_permission()) return 10;
    return 0;
}
''')
    expected["caps_production_pico_sequence"] = 0
    return "\n".join(lines), expected


class CapsPolicyTests(unittest.TestCase):
    def test_actual_c_policy_freestanding(self):
        source, expected = policy_probe_source()
        # Test-local imports do not invoke OS services. The executable has no
        # imports and does not load Android/VR.
        source += '\nint strcmp(const char* a, const char* b) { while (*a && *a == *b) { a++; b++; } return (unsigned char)*a - (unsigned char)*b; }\n'
        source += 'int entry(void) {\n'
        for i, (name, value) in enumerate(expected.items(), 1):
            source += f'if ({name}() != {value}) return {i};\n'
        source += 'return 0; }\n'
        with tempfile.TemporaryDirectory(prefix="vdhs-caps-", dir=Path(os.environ["TEMP"]) / "opencode") as temp:
            source_path = Path(temp) / "caps_policy_probe.c"
            obj_path = Path(temp) / "caps_policy_probe.obj"
            exe_path = Path(temp) / "caps_policy_probe.exe"
            source_path.write_text(source, encoding="utf-8")
            (Path(temp) / "string.h").write_text("int strcmp(const char*, const char*);\n", encoding="utf-8")
            (Path(temp) / "pthread.h").write_text(
                "typedef int pthread_once_t; typedef int pthread_mutex_t;\n"
                "#define PTHREAD_ONCE_INIT 0\n#define PTHREAD_MUTEX_INITIALIZER 0\n"
                "int pthread_once(pthread_once_t*, void (*)(void));\n"
                "int pthread_mutex_lock(pthread_mutex_t*); int pthread_mutex_unlock(pthread_mutex_t*);\n",
                encoding="utf-8")
            (Path(temp) / "sys").mkdir()
            (Path(temp) / "sys/system_properties.h").write_text(
                "#define PROP_VALUE_MAX 92\nint __system_property_get(const char*, char*);\n", encoding="utf-8")
            (Path(temp) / "android").mkdir()
            (Path(temp) / "android/log.h").write_text(
                "#define ANDROID_LOG_INFO 4\nint __android_log_print(int, const char*, const char*, ...);\n", encoding="utf-8")
            subprocess.run([str(CLANG), "--target=x86_64-pc-windows-msvc", "-ffreestanding",
                            "-fno-stack-protector", "-O2", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-I", temp, "-I", str(NATIVE), "-c", str(source_path), "-o", str(obj_path)], check=True)
            subprocess.run([str(CLANG.with_name("ld.lld.exe")), "-flavor", "link", "/entry:entry",
                            "/subsystem:console", "/nodefaultlib", "/out:" + str(exe_path), str(obj_path)], check=True)
            result = subprocess.run([str(exe_path)], check=False, timeout=10)
        names = list(expected)
        failed = names[result.returncode - 1] if 0 < result.returncode <= len(names) else result.returncode
        self.assertEqual(result.returncode, 0, f"policy assertion: {failed}")
        self.assertEqual(len(expected), len(CASES) + 16)

    def test_native_files_compile_without_warnings(self):
        include = NATIVE / "third_party/shadowhook-2.0.1/cpp/include"
        for name in ("vdhs_caps.c", "vdhs_layer.c", "vdhs_hand.c"):
            with self.subTest(file=name):
                completed = subprocess.run([str(CLANG), *FLAGS, "-I", str(include), "-fsyntax-only", str(NATIVE / name)],
                                           capture_output=True, text=True)
                self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
                self.assertEqual(completed.stderr, "")

    def test_mutations_are_guarded_but_hand_paths_are_not(self):
        hand = (NATIVE / "vdhs_hand.c").read_text(encoding="utf-8")
        layer = (NATIVE / "vdhs_layer.c").read_text(encoding="utf-8")
        self.assertIn("if (!vdhs_caps_fix_face_permission()) return 0;", hand)
        self.assertIn("if (!vdhs_caps_eye_supported()) return 0;", hand)
        self.assertIn("if (!vdhs_caps_eye_supported() || g_saved_eye_gaze != 1) return;", hand)
        face_fix = hand[hand.index("int vdhs_hand_fix_permissions("):hand.index("static int g_saved_eye_gaze")]
        self.assertNotIn("vdhs_caps_eye_supported()", face_fix)
        self.assertIn('strcmp(face, "com.oculus.permission.FACE_TRACKING")', face_fix)
        self.assertIn('prop_set(ds, def, "FaceTrackingPermission", s)', face_fix)
        self.assertIn("if (!perm_done && vdhs_caps_fix_face_permission())", layer)
        locate = layer[layer.index("static XrResult hk_locate_space("):layer.index("static XrResult hk_create_action(")]
        self.assertLess(locate.index("!vdhs_caps_eye_supported()"), locate.index("gaze_caller()"))
        self.assertLess(locate.index("caller != GAZE_CALLER_POINTER"), locate.index("gaze_filter(base"))
        self.assertLess(locate.index("caller != GAZE_CALLER_POINTER"), locate.index("gaze_vd_fix(base"))
        create = layer[layer.index("static XrResult hk_create_instance("):layer.index("static XrResult hk_gipa(")]
        self.assertIn("if (hand_added) names[m++] = kHandExt;", create)
        self.assertIn("if (vdhs_hand_mesh_ready()) vals[nv++] = kExtEnumFbMesh;", create)
        self.assertNotIn("vdhs_caps_eye_supported()", create)
        caps = (NATIVE / "vdhs_caps.c").read_text(encoding="utf-8")
        self.assertNotIn("ro.product", caps)
        self.assertNotIn("g_face", caps)


if __name__ == "__main__":
    unittest.main()
