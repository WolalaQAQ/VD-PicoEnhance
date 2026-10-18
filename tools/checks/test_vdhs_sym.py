"""Windows offline xDL mock/source contracts and real NDK compile/link checks.

Run with: python -B -m unittest discover -s tools/checks -p '*sym*.py' -v
The host DLL executes the production resolver against mock mapped objects;
neither it nor the Android compile checks validate device namespaces/timing.
"""

import ctypes
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
NATIVE = ROOT / 'mod/native'
XDL = NATIVE / 'third_party/shadowhook-2.0.1/cpp/third_party/xdl'
SDK = Path(os.environ.get('ANDROID_SDK_ROOT') or os.environ.get('ANDROID_HOME')
           or (ROOT / 'tools/sdk-path.txt').read_text(encoding='utf-8').strip())
NDK = Path(os.environ.get('ANDROID_NDK_HOME', SDK / 'ndk/26.1.10909125'))
BIN = NDK / 'toolchains/llvm/prebuilt/windows-x86_64/bin'
TEMP_ROOT = Path(os.environ['LOCALAPPDATA']) / 'Temp/opencode'

# These declarations are only for the Windows mock. Android checks below use
# the vendored xdl.h and NDK headers, never this shim.
MOCK_XDL_H = r'''
#ifndef VDHS_SYM_MOCK_XDL_H
#define VDHS_SYM_MOCK_XDL_H
typedef __SIZE_TYPE__ size_t;
#define NULL ((void*)0)
#define XDL_DEFAULT 0
struct dl_phdr_info {
    unsigned long long dlpi_addr;
    const char* dlpi_name;
    const void* dlpi_phdr;
    unsigned short dlpi_phnum;
};
void* xdl_open2(struct dl_phdr_info*);
void* xdl_sym(void*, const char*, size_t*);
void* xdl_close(void*);
int xdl_iterate_phdr(int (*)(struct dl_phdr_info*, size_t, void*), void*, int);
#endif
'''
MOCK_STRING_H = r'''
#include "xdl.h"
size_t strlen(const char*);
int strcmp(const char*, const char*);
void* memset(void*, int, size_t);
'''


def run(*args):
    result = subprocess.run([str(a) for a in args], cwd=ROOT,
                            capture_output=True, text=True)
    if result.returncode:
        raise AssertionError(f'{args}\n{result.stdout}{result.stderr}')
    return result.stdout


def c_code(path):
    return re.sub(r'/\*.*?\*/|//[^\n]*', '', path.read_text(encoding='utf-8'),
                  flags=re.S)


class SymMockTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='vdhs-sym-', dir=TEMP_ROOT)
        cls.addClassCleanup(cls.temp.cleanup)
        temp = Path(cls.temp.name)
        (temp / 'xdl.h').write_text(MOCK_XDL_H, encoding='utf-8')
        (temp / 'string.h').write_text(MOCK_STRING_H, encoding='utf-8')
        objects = []
        for source in (NATIVE / 'vdhs_sym.c', Path(__file__).with_name('vdhs_sym_mock.c')):
            obj = temp / (source.stem + '.obj')
            run(BIN / 'clang.exe', '--target=x86_64-pc-windows-msvc', '-std=c11',
                '-Wall', '-Wextra', '-Werror', '-nostdinc', '-fno-builtin',
                '-fno-stack-protector', '-I', temp, '-c', source, '-o', obj)
            objects.append(obj)
        dll = temp / 'vdhs_sym_mock.dll'
        run(BIN / 'ld.lld.exe', '-flavor', 'link', '/dll', '/noentry',
            '/nodefaultlib', '/machine:x64', f'/out:{dll}',
            '/export:vdhs_resolve_mapped_symbol',
            '/export:vdhs_resolve_mapped_symbols', *objects)
        cls.lib = ctypes.CDLL(str(dll))
        # Explicit unload lets TemporaryDirectory remove the DLL on Windows.
        kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        kernel.FreeLibrary.argtypes = [ctypes.c_void_p]
        kernel.FreeLibrary.restype = ctypes.c_int
        cls.addClassCleanup(lambda: kernel.FreeLibrary(cls.lib._handle))
        cls.lib.sym_mock_add.argtypes = [ctypes.c_char_p, ctypes.c_int]
        cls.lib.sym_mock_add.restype = ctypes.c_int
        cls.lib.sym_mock_symbol.argtypes = [ctypes.c_int, ctypes.c_char_p,
                                            ctypes.c_ulonglong]
        cls.lib.sym_mock_symbol.restype = None
        cls.lib.sym_mock_reset.argtypes = []
        cls.lib.sym_mock_reset.restype = None
        cls.lib.vdhs_resolve_mapped_symbol.argtypes = [ctypes.c_char_p, ctypes.c_char_p]
        cls.lib.vdhs_resolve_mapped_symbol.restype = ctypes.c_void_p
        cls.lib.vdhs_resolve_mapped_symbols.argtypes = [
            ctypes.c_char_p, ctypes.POINTER(ctypes.c_char_p),
            ctypes.POINTER(ctypes.c_void_p), ctypes.c_int]
        cls.lib.vdhs_resolve_mapped_symbols.restype = ctypes.c_int

    def setUp(self):
        self.lib.sym_mock_reset()

    def add(self, path, symbols=(), can_open=True):
        index = self.lib.sym_mock_add(path.encode(), can_open)
        for name, value in symbols:
            self.lib.sym_mock_symbol(index, name.encode(), value)
        return index

    def stat(self, name):
        return ctypes.c_int.in_dll(self.lib, 'sym_mock_' + name).value

    def resolve(self, lib, names=('one', 'two'), existing=(None, None)):
        array = (ctypes.c_char_p * len(names))(*(n.encode() for n in names))
        out = (ctypes.c_void_p * len(names))(*existing)
        count = self.lib.vdhs_resolve_mapped_symbols(lib.encode(), array, out, len(names))
        return count, list(out)

    def test_one_handle_for_entire_batch(self):
        self.add('/data/app/vd/lib/arm64/libopenxr_loader.so', [('one', 11), ('two', 22)])
        self.assertEqual(self.resolve('/lib/arm64/libopenxr_loader.so'), (2, [11, 22]))
        self.assertEqual([self.stat(n) for n in ('opens', 'closes', 'queries')], [1, 1, 2])
        self.assertEqual(self.stat('flags'), 0)

    def test_no_cross_library_symbol_fill(self):
        self.add('/data/a/libtarget.so', [('one', 11)])
        self.add('/data/b/libtarget.so', [('two', 22)])
        self.add('/data/other.so', [('one', 33), ('two', 44)])
        self.assertEqual(self.resolve('libtarget.so'), (1, [11, None]))
        self.assertEqual(self.stat('opens'), 1)

    def test_empty_first_match_does_not_try_other_target(self):
        self.add('/data/a/libtarget.so')
        self.add('/data/b/libtarget.so', [('one', 11), ('two', 22)])
        self.assertEqual(self.resolve('libtarget.so'), (0, [None, None]))
        self.assertEqual(self.stat('opens'), 1)
        self.assertEqual(self.stat('closes'), 1)

    def test_open_failure_does_not_try_other_target(self):
        self.add('/data/a/libtarget.so', can_open=False)
        self.add('/data/b/libtarget.so', [('one', 11)])
        self.assertEqual(self.resolve('libtarget.so', existing=(99, None)), (0, [99, None]))
        self.assertEqual([self.stat(n) for n in ('opens', 'closes', 'queries')], [1, 0, 0])

    def test_path_selector_never_falls_back_to_basename(self):
        self.add('/system/lib64/libopenxr_loader.so', [('one', 11)])
        self.add('/data/vd/lib/arm64-v8a/libopenxr_loader.so', [('two', 22)])
        self.assertEqual(self.resolve('/lib/arm64/libopenxr_loader.so'), (0, [None, None]))
        self.assertEqual(self.stat('opens'), 0)

    def test_loader_suffix_selects_target_after_unrelated_same_basename(self):
        self.add('/system/lib64/libopenxr_loader.so', [('one', 11), ('two', 22)])
        self.add('/data/vd/lib/arm64/libopenxr_loader.so', [('one', 33), ('two', 44)])
        self.assertEqual(self.resolve('/lib/arm64/libopenxr_loader.so'), (2, [33, 44]))
        self.assertEqual(self.stat('opens'), 1)

    def test_component_and_terminal_boundaries(self):
        self.add('/data/notlibtarget.so', [('one', 11)])
        self.add('/data/libtarget.so.bak', [('one', 22)])
        self.add('/data/libtarget.so/different.so', [('one', 33)])
        self.assertEqual(self.resolve('libtarget.so'), (0, [None, None]))
        self.assertEqual(self.stat('opens'), 0)

    def test_apk_member_basename_and_explicit_path(self):
        path = '/data/app/vd/base.apk!/lib/arm64-v8a/libmonosgen-2.0.so'
        self.add(path, [('one', 11), ('two', 22)])
        for selector in ('libmonosgen-2.0.so', '/lib/arm64-v8a/libmonosgen-2.0.so',
                         'base.apk!/lib/arm64-v8a/libmonosgen-2.0.so', path):
            with self.subTest(selector=selector):
                self.assertEqual(self.resolve(selector), (2, [11, 22]))

    def test_unrelated_apks_are_not_scanned_for_symbols(self):
        self.add('/data/other.apk!/lib/arm64-v8a/other.so', [('one', 11), ('two', 22)])
        self.add('/data/vd.apk!/lib/arm64-v8a/libtarget.so', [('one', 33)])
        self.assertEqual(self.resolve('/lib/arm64-v8a/libtarget.so'), (1, [33, None]))
        self.assertEqual(self.stat('opens'), 1)

    def test_no_anonymous_apk_member_guess(self):
        self.add('/data/app/vd/base.apk', [('one', 11)])
        self.add('[anon:apk-elf]', [('one', 22)])
        self.assertEqual(self.resolve('libtarget.so'), (0, [None, None]))
        self.assertEqual(self.stat('queries'), 0)

    def test_prefilled_outputs_preserved_and_counted(self):
        self.add('/data/libtarget.so', [('one', 11), ('two', 22)])
        self.assertEqual(self.resolve('libtarget.so', existing=(99, None)), (2, [99, 22]))
        self.assertEqual(self.stat('queries'), 1)
        self.assertEqual(self.resolve('libtarget.so', existing=(99, 88)), (2, [99, 88]))
        self.assertEqual(self.stat('queries'), 1)

    def test_absent_target_keeps_outputs_but_returns_zero(self):
        self.add('/data/other.so', [('two', 22)])
        self.assertEqual(self.resolve('libtarget.so', existing=(99, None)), (0, [99, None]))
        self.assertEqual([self.stat(n) for n in ('opens', 'closes', 'queries')], [0, 0, 0])

    def test_no_cache_after_runtime_reload(self):
        self.add('/system/lib64/libopenxr_forwardloader.so', [('one', 11)])
        single = self.lib.vdhs_resolve_mapped_symbol
        self.assertEqual(single(b'libopenxr_forwardloader.so', b'one'), 11)
        self.lib.sym_mock_reset()
        self.assertIsNone(single(b'libopenxr_forwardloader.so', b'one'))
        self.add('/system/lib64/libopenxr_forwardloader.so', [('one', 22)])
        self.assertEqual(single(b'libopenxr_forwardloader.so', b'one'), 22)

    def test_single_symbol_missing_returns_null(self):
        self.add('/data/libtarget.so', [('one', 11)])
        self.assertIsNone(self.lib.vdhs_resolve_mapped_symbol(b'libtarget.so', b'missing'))
        self.assertEqual(self.stat('closes'), 1)


class SymContractTests(unittest.TestCase):
    def test_source_is_thin_mapped_only_xdl_adapter(self):
        code = c_code(NATIVE / 'vdhs_sym.c')
        for forbidden in (r'\bdlopen\s*\(', r'\bdlsym\s*\(', r'\bxdl_open\s*\(',
                          r'\bxdl_dsym\s*\(', 'FORCE_LOAD', '/proc/', r'\bfopen\s*\(',
                          'ElfW', 'DT_SYMTAB', 'DT_GNU_HASH'):
            with self.subTest(forbidden=forbidden):
                self.assertNotRegex(code, forbidden)
        self.assertIn('xdl_iterate_phdr(resolve_in_object, &q, XDL_DEFAULT)', code)
        self.assertEqual(len(re.findall(r'\bxdl_open2\s*\(', code)), 1)
        self.assertEqual(len(re.findall(r'\bxdl_close\s*\(', code)), 1)
        self.assertIn('xdl_sym(handle, q->names[i], NULL)', code)

    def test_payload_uses_shared_entry_and_mark_remains_weak(self):
        payload = c_code(NATIVE / 'vdhs_payload.c')
        self.assertIn('#include "vdhs_sym.h"', payload)
        self.assertIn('vdhs_resolve_mapped_symbols(kMonoLib, kSyms, syms, kSymCount)', payload)
        for removed in ('find_mapped_base', 'gnu_symcount', 'static int resolve_mapped_symbols'):
            self.assertNotIn(removed, payload)
        self.assertRegex(c_code(NATIVE / 'vdhs_mark.c'),
                         r'extern void\* vdhs_resolve_mapped_symbol\([^;]+__attribute__\(\(weak\)\)')

    def test_ndk_arm64_compile_and_standalone_mark_link(self):
        with tempfile.TemporaryDirectory(prefix='vdhs-sym-ndk-', dir=TEMP_ROOT) as work:
            temp = Path(work)
            common = [BIN / 'clang.exe', '--target=aarch64-linux-android29',
                      '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
                      '-fPIC']
            for source in ('vdhs_sym.c', 'vdhs_payload.c'):
                run(*common, '-I', XDL, '-c', NATIVE / source, '-o', temp / (source + '.o'))
            # This links vdhs_mark.c alone: adding the shared resolver must not
            # turn its optional Zygisk lookup into a mandatory xDL dependency.
            standalone = temp / 'libvdhs_sym_mark_standalone.so'
            run(*common, '-shared', NATIVE / 'vdhs_mark.c', '-llog', '-ldl',
                '-Wl,--no-undefined', '-o', standalone)
            symbols = run(BIN / 'llvm-nm.exe', '-D', standalone)
            self.assertRegex(symbols, r'\bw vdhs_resolve_mapped_symbol\b')


if __name__ == '__main__':
    unittest.main()
