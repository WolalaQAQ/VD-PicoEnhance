"""Offline checks. No real sockets, device, Android VM or Binder are invoked."""
import argparse
import ctypes as C
import json
import os
from pathlib import Path
import subprocess
import unittest
import zipfile

HERE = Path(__file__).resolve().parent
OUTPUT = HERE / 'build'
OUT = OUTPUT / 'checks'
SOURCE = HERE / 'src'
ORIGINAL = HERE.parents[1] / 'mixedvr/headset'
STATE = '/data/adb/pico_mixedvr_standalone'
LLVM = Path(os.environ['ANDROID_SDK_ROOT']) / 'ndk/26.1.10909125/toolchains/llvm/prebuilt/windows-x86_64/bin'
SID = 'b' * 32

def ip(*parts):
    return '.'.join(map(str, parts)).encode('ascii')

def pair(left, right):
    return ip(*left) + b' ' + ip(*right)


def section(text, begin, end):
    return text[text.index(begin):text.index(end)]


class StandaloneTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        OUT.mkdir(parents=True, exist_ok=True)
        cls.source = (SOURCE / 'mixedvrd.c').read_text(encoding='utf-8')
        cls.original = (ORIGINAL / 'mixedvrd.c').read_text(encoding='utf-8')
        (OUT / 'core_source.h').write_text(section(cls.source, 'static bool journal(void)', 'static void reply('), encoding='utf-8')
        (OUT / 'reply_source.h').write_text(section(cls.source, 'static void reply(', '#include "lan_config.h"'), encoding='utf-8')
        (OUT / 'serve_source.h').write_text(section(cls.source, 'static void startup_error(', 'static int supervise(void)'), encoding='utf-8')
        obj, dll = OUT / 'host.obj', OUT / 'host.dll'
        commands = [
            [str(LLVM / 'clang.exe'), '--target=x86_64-pc-windows-msvc', '-std=c11',
             '-ffreestanding', '-fno-builtin', '-fno-stack-protector', '-O2', '-Wall', '-Wextra', '-Werror',
             '-I', str(OUT), '-I', str(SOURCE), '-c', str(HERE / 'checks/host.c'), '-o', str(obj)],
            [str(LLVM / 'ld.lld.exe'), '-flavor', 'link', '/dll', '/noentry', '/nodefaultlib',
             '/machine:x64', '/wx', '/out:' + str(dll), str(obj)],
        ]
        for index, command in enumerate(commands):
            result = subprocess.run(command, capture_output=True, text=True, encoding='utf-8', timeout=120)
            (OUT / f'compile-{index}.log').write_text(subprocess.list2cmdline(command) + '\n' + result.stdout + result.stderr, encoding='utf-8')
            if result.returncode:
                raise RuntimeError(result.stdout + result.stderr)
        cls.lib = C.CDLL(str(dll))
        cls.crt = C.CDLL('ucrtbase.dll')
        names = ['__stdio_common_vsprintf', '__stdio_common_vsscanf', 'strcmp', 'strlen',
                 'memcpy', 'memmove', 'memchr', 'strtoull', 'atoi', '_errno']
        pointers = (C.c_void_p * len(names))(*(C.cast(getattr(cls.crt, n), C.c_void_p).value for n in names))
        cls.lib.set_crt.argtypes = [C.POINTER(C.c_void_p)]
        cls.lib.set_crt(pointers)
        cls.lib.parse_network.argtypes = [C.c_char_p]
        cls.lib.add.argtypes = [C.c_uint64, C.c_int, C.c_char_p]
        cls.lib.control.argtypes = [C.c_int, C.c_uint64]
        cls.lib.value.argtypes = [C.c_int]
        cls.lib.value.restype = C.c_uint64
        cls.lib.answers.restype = C.c_char_p
        cls.lib.startup_log.restype = C.c_char_p
        cls.lib.discovery_answer.restype = C.c_char_p
        kernel = C.WinDLL('kernel32', use_last_error=True)
        kernel.FreeLibrary.argtypes, kernel.FreeLibrary.restype = [C.c_void_p], C.c_int
        def unload():
            if not kernel.FreeLibrary(cls.lib._handle):
                raise C.WinError(C.get_last_error())
        cls.addClassCleanup(unload)

    def setUp(self):
        self.lib.reset(1)

    def tearDown(self):
        self.assertEqual(self.lib.value(14), 0, 'unexpected path, operation or mock call')

    def event(self, time, action, text=''):
        self.lib.add(time, action, text.encode())

    def request(self, seq, command, *, time=10100, lease=3000, token='a'*64, sid=SID):
        self.event(time, 2, f'VDM1 {token} {sid} {seq} {command} {lease}\n')

    def connect(self):
        self.event(10000, 1)

    def assert_start_failure(self, stage, error):
        self.assertEqual(self.lib.run(), 1)
        for counter in (3, 4, 6, 7, 8, 9, 10, 12, 19):
            self.assertEqual(self.lib.value(counter), 0, 'failed startup must not publish readiness or take system ownership')
        log = self.lib.startup_log()
        self.assertEqual(log, f'pico-mixedvr t_ms=10000 pid=43 step={stage} errno={error}\n'.encode())
        self.assertNotIn(ip(10,0,0,2), log)
        self.assertNotIn(b'a' * 64, log)
        (OUT / f'startup-{stage}.log').write_bytes(log)

    def test_accepts_private_interface_and_pc_pair(self):
        for value in (pair((10,0,0,2),(10,0,0,3)), pair((172,16,0,2),(172,31,255,254)), pair((192,168,1,2),(192,168,1,3))):
            self.assertTrue(self.lib.parse_network(value))

    def test_rejects_public_wildcard_loopback_and_malformed_config(self):
        left, right = ip(10,0,0,2), ip(10,0,0,3)
        for value in (pair((0,0,0,0),(10,0,0,3)), pair((127,0,0,1),(10,0,0,3)),
                      pair((10,0,0,2),(8,8,8,8)), pair((172,32,0,1),(10,0,0,3)),
                      left + b' ' + left, ip(10,0,0,256) + b' ' + right,
                      ip(10,0,0) + b' ' + right, left + b' ' + right + b' extra',
                      left + b'\t' + right, left + b'  ' + right, left + b' ' + right + b'\n', b''):
            with self.subTest(pair=value): self.assertFalse(self.lib.parse_network(value))

    def test_real_loop_binds_explicit_private_address_not_loopback(self):
        self.connect(); self.request(1, 'status')
        self.assertEqual(self.lib.run(), 0)
        self.assertEqual(self.lib.value(15), 0x0a000002)
        self.assertEqual(self.lib.value(16), 29744)
        self.assertIn(b'VDR1 1 0 1 unobserved ok\n', self.lib.answers())
        self.assertEqual(self.lib.value(6), 0)

    def test_missing_network_config_does_not_open_listener(self):
        self.lib.option(4, 0)
        self.assertEqual(self.lib.run(), 1)
        self.assertEqual(self.lib.value(16), 0)

    def test_unassigned_address_requires_freebind_in_kernel_model(self):
        self.lib.option(11, 0)
        self.assertEqual(self.lib.bind_without_freebind(), -1)
        self.assertEqual(self.lib.value(24), 99)
        self.assertEqual(self.lib.value(18), 0)
        self.assertEqual(self.lib.value(19), 0)

    def test_unassigned_address_starts_exact_listener_with_freebind_without_pc(self):
        self.lib.option(11, 0)
        self.assertEqual(self.lib.run(), 0)
        self.assertEqual(self.lib.value(18), 1)
        self.assertEqual(self.lib.value(19), 1)
        self.assertEqual(self.lib.value(20), 1)
        self.assertEqual(self.lib.value(21), 1)
        self.assertEqual(self.lib.value(15), 0x0a000002)
        self.assertEqual(self.lib.value(16), 29744)
        for counter in (3, 4, 6, 7, 8, 9, 10, 12):
            self.assertEqual(self.lib.value(counter), 0)

    def test_socket_failure_does_not_publish_ready_or_take_ownership(self):
        self.lib.option(14, 1)
        self.assert_start_failure('socket', 24)
        self.assertEqual(self.lib.value(21), 0)
        self.assertEqual(self.lib.value(23), 0)

    def test_freebind_failure_does_not_bind_or_publish_ready(self):
        self.lib.option(12, 1)
        self.assert_start_failure('freebind', 1)
        self.assertEqual(self.lib.value(18), 0)
        self.assertEqual(self.lib.value(20), 0)
        self.assertEqual(self.lib.value(23), 1)

    def test_freebind_does_not_bypass_port_conflict(self):
        self.lib.option(11, 0)
        self.lib.option(13, 1)
        self.assert_start_failure('bind', 98)
        self.assertEqual(self.lib.value(18), 1)
        self.assertEqual(self.lib.value(20), 1)
        self.assertEqual(self.lib.value(22), 0)
        self.assertEqual(self.lib.value(23), 1)

    def test_listen_failure_does_not_publish_ready_or_take_ownership(self):
        self.lib.option(15, 1)
        self.assert_start_failure('listen', 5)
        self.assertEqual(self.lib.value(22), 1)
        self.assertEqual(self.lib.value(23), 1)

    def test_ready_failure_does_not_publish_ready_or_take_ownership(self):
        self.lib.option(16, 1)
        self.assert_start_failure('ready', 5)
        self.assertEqual(self.lib.value(22), 1)
        self.assertEqual(self.lib.value(23), 1)

    def test_other_private_peer_cannot_occupy_client_slot(self):
        self.event(10000, 7)
        self.request(1, 'index')
        self.assertEqual(self.lib.run(), 0)
        self.assertEqual(self.lib.value(13), 0)
        self.assertEqual(self.lib.value(6), 0)

    def test_original_fixed_protocol_full_mode_cycle_and_disconnect(self):
        self.connect(); self.request(1, 'status')
        self.request(2, 'index', time=10200)
        self.request(3, 'hands', time=10300)
        self.request(4, 'restore', time=10400)
        self.event(10500, 3)
        self.assertEqual(self.lib.run(), 0)
        self.assertEqual(self.lib.answers().splitlines(), [
            b'VDR1 1 0 1 unobserved ok', b'VDR1 2 1 0 unobserved ok',
            b'VDR1 3 2 1 unobserved ok', b'VDR1 4 0 1 unobserved ok'])
        self.assertEqual(self.lib.value(6), 1)
        self.assertEqual(self.lib.value(3), 0)

    def discover(self, *, time=10000, action=11, key='a'*64, nonce='b'*32):
        self.event(time, action, f'PKD1 {key} {nonce}')

    def test_unicast_discovery_echoes_nonce_without_system_control(self):
        self.discover(action=9)
        self.assertEqual(self.lib.run(), 0)
        self.assertEqual(self.lib.discovery_answer(), b'PKA1 ' + b'a'*64 + b' ' + b'b'*32)
        self.assertEqual(self.lib.value(25), 1)
        self.assertEqual(self.lib.value(6), 0)
        self.assertEqual(self.lib.value(7), 0)

    def test_multicast_discovery_allows_paired_pc_at_new_ip(self):
        self.discover()
        self.event(10020, 12)
        self.request(1, 'status', time=10040)
        self.assertEqual(self.lib.run(), 0)
        self.assertEqual(self.lib.value(25), 1)
        self.assertIn(b'VDR1 1 0 1 unobserved ok', self.lib.answers())
        self.assertEqual(self.lib.value(6), 0)

    def test_bad_discovery_key_never_admits_new_pc(self):
        self.discover(key='c'*64)
        self.event(10020, 12)
        self.request(1, 'index', time=10040)
        self.assertEqual(self.lib.run(), 0)
        self.assertEqual(self.lib.value(25), 0)
        self.assertEqual(self.lib.value(13), 0)
        self.assertEqual(self.lib.value(6), 0)

    def test_malformed_or_other_protocol_discovery_is_ignored(self):
        for text in ('DISCOVER_DAEMON', 'PKD1 ' + 'a'*64 + ' ' + 'x'*32,
                     'PKD1 ' + 'a'*64 + ' ' + 'b'*32 + '\n', 'PKA1 ' + 'a'*64 + ' ' + 'b'*32):
            self.lib.reset(1)
            self.event(10000, 11, text)
            self.assertEqual(self.lib.run(), 0)
            self.assertEqual(self.lib.value(25), 0)

    def test_discovery_does_not_accept_a_different_subnet(self):
        self.discover(action=13)
        self.assertEqual(self.lib.run(), 0)
        self.assertEqual(self.lib.value(25), 0)

    def test_discovered_pc_permission_expires(self):
        self.discover()
        self.event(16001, 12)
        self.request(1, 'index', time=16020)
        self.assertEqual(self.lib.run(), 0)
        self.assertEqual(self.lib.value(13), 0)
        self.assertEqual(self.lib.value(6), 0)

    def test_discovery_never_replaces_an_active_client(self):
        self.connect(); self.request(1, 'index')
        self.discover(time=10200)
        self.event(10300, 8)
        self.discover(time=10400)
        self.event(10420, 12)
        self.request(1, 'status', time=10440)
        self.assertEqual(self.lib.run(), 0)
        self.assertEqual(self.lib.value(25), 1)
        self.assertEqual(self.lib.value(6), 1)
        self.assertIn(b'VDR1 1 0 1 unobserved ok', self.lib.answers())

    def test_headset_address_change_restores_then_rebinds_without_republishing_ready(self):
        self.connect(); self.request(1, 'index')
        self.event(11100, 10)
        self.event(11200, 4)
        self.assertEqual(self.lib.run(), 0)
        self.assertEqual(self.lib.value(15), 0x0a000007)
        self.assertEqual(self.lib.value(19), 1)
        self.assertEqual(self.lib.value(20), 2)
        self.assertEqual(self.lib.value(6), 1)
        self.assertEqual(self.lib.value(1), 1)
        self.assertEqual(self.lib.value(3), 0)

    def test_failed_address_change_recovery_keeps_journal_and_refuses_discovery(self):
        self.lib.option(1, 1)
        self.connect(); self.request(1, 'index')
        self.event(11100, 10)
        self.discover(time=11200)
        self.assertEqual(self.lib.run(), 2)
        self.assertEqual(self.lib.value(25), 0)
        self.assertEqual(self.lib.value(3), 1)
        self.assertEqual(self.lib.value(6), 1)

    def test_rebind_failure_keeps_service_alive_and_retries_current_address(self):
        self.connect(); self.request(1, 'index')
        self.event(11100, 20)
        self.event(12200, 21)
        self.event(13300, 4)
        self.assertEqual(self.lib.run(), 0)
        self.assertEqual(self.lib.value(15), 0x0a000007)
        self.assertEqual(self.lib.value(22), 2)
        self.assertEqual(self.lib.value(19), 1)
        self.assertEqual(self.lib.value(1), 1)

    def test_public_interface_address_is_not_bound(self):
        self.event(11100, 14)
        self.assertEqual(self.lib.run(), 0)
        self.assertEqual(self.lib.value(15), 0x0a000002)
        self.assertEqual(self.lib.value(20), 1)

    def test_second_address_change_during_failed_rebind_uses_latest_wlan_address(self):
        self.event(11100, 20)
        self.event(12200, 22)
        self.event(13300, 4)
        self.assertEqual(self.lib.run(), 0)
        self.assertEqual(self.lib.value(15), 0x0a000009)
        self.assertEqual(self.lib.value(22), 2)
        self.assertEqual(self.lib.value(19), 1)

    def test_discovery_unavailable_keeps_legacy_paired_tcp_working(self):
        self.lib.option(17, 1)
        self.connect(); self.request(1, 'index')
        self.event(10200, 8)
        self.assertEqual(self.lib.run(), 0)
        self.assertEqual(self.lib.value(6), 1)
        self.assertEqual(self.lib.value(1), 1)

    def test_bad_token_does_not_capture_baseline(self):
        self.connect(); self.request(1, 'index', token='c'*64)
        self.lib.run()
        self.assertIn(b' error\n', self.lib.answers())
        self.assertEqual(self.lib.value(6), 0)

    def test_unknown_command_not_root_execution(self):
        self.connect(); self.request(1, 'reboot')
        self.lib.run()
        self.assertIn(b' error\n', self.lib.answers())
        self.assertEqual(self.lib.value(6), 0)

    def test_lease_bounds_and_sequence_overflow_rejected(self):
        for seq, lease in ((1, 499), (1, 5001), (18446744073709551616, 3000)):
            self.lib.reset(1)
            self.connect(); self.request(seq, 'index', lease=lease)
            self.lib.run()
            self.assertIn(b' error\n', self.lib.answers())
            self.assertEqual(self.lib.value(6), 0)

    def test_session_change_is_not_a_second_owner(self):
        self.connect(); self.request(1, 'index')
        self.request(2, 'hands', time=10200, sid='c'*32)
        self.lib.run()
        self.assertEqual(self.lib.value(6), 1)
        self.assertEqual(self.lib.value(1), 1)

    def test_duplicate_sequence_restores_instead_of_reapplying(self):
        self.connect(); self.request(1, 'index')
        self.request(1, 'hands', time=10200)
        self.lib.run()
        self.assertIn(b'VDR1 1 1 0 unobserved error', self.lib.answers())
        self.assertEqual(self.lib.value(6), 1)
        self.assertEqual(self.lib.value(1), 1)

    def test_status_does_not_extend_mode_lease(self):
        self.connect(); self.request(1, 'index', lease=500)
        self.request(2, 'status', time=10300)
        self.event(10700, 4)
        self.request(3, 'hands', time=10800)
        self.lib.run()
        self.assertEqual(self.lib.value(6), 1)
        self.assertEqual(self.lib.value(1), 1)
        self.assertEqual(self.lib.value(13), 2)

    def test_hangup_restores_original_zero(self):
        self.lib.reset(0)
        self.connect(); self.request(1, 'hands')
        self.event(10200, 8)
        self.lib.run()
        self.assertEqual(self.lib.value(1), 0)
        self.assertEqual(self.lib.value(2), 0)
        self.assertEqual(self.lib.value(3), 0)

    def test_disabling_daemon_restores_original_one(self):
        self.connect(); self.request(1, 'index')
        self.event(10200, 6)
        self.lib.run()
        self.assertEqual(self.lib.value(1), 1)
        self.assertEqual(self.lib.value(3), 0)

    def test_original_recovery_core_is_identical(self):
        for start, end in [('static bool journal(void)', 'static void reply('),
                           ('static bool mark_inflight(void)', 'static void export_backend(')]:
            self.assertEqual(section(self.source, start, end), section(self.original, start, end))

    def test_java_binder_json_and_firmware_checks_preserved(self):
        for name in ('ConfigurationControl.java', 'TrackingControl.java', 'ControlSession.java'):
            source = (SOURCE / 'java/org/vdpico/mixedvr' / name).read_text(encoding='utf-8')
            original = (ORIGINAL / 'java/org/vdpico/mixedvr' / name).read_text(encoding='utf-8')
            self.assertEqual(source, original.replace('/data/adb/vdhs_mixedvr', STATE))

    def test_no_vd_private_file_or_process_dependency_in_binary(self):
        data = (HERE / 'build/module/bin/pico-mixedvrd').read_bytes()
        for forbidden in (b'VirtualDesktop.Android', b'mixedvr_request.txt', b'mixedvr_status.txt',
                          b'/data/data', b'local_control', b'VDHS_PIPE_'):
            self.assertNotIn(forbidden, data)
        for forbidden in ('local_tick(', 'local_read(', 'local_control.', 'local_drop('):
            self.assertNotIn(forbidden, self.source)

    def test_build_contains_arm64_elf_and_dex_not_host_stubs(self):
        data = (HERE / 'build/module/bin/pico-mixedvrd').read_bytes()
        self.assertEqual(data[:5], b'\x7fELF\x02')
        self.assertEqual(int.from_bytes(data[18:20], 'little'), 183)
        with zipfile.ZipFile(HERE / 'build/module/bin/tracking-control.jar') as jar:
            self.assertTrue(jar.read('classes.dex').startswith(b'dex\n'))

    def test_restore_failure_keeps_original_journal_and_retries(self):
        self.assertTrue(self.lib.control(1, 3000))
        self.lib.option(1, 1)
        self.assertFalse(self.lib.recover())
        self.assertEqual(self.lib.value(3), 1)
        self.assertEqual(self.lib.value(4), 1)
        self.lib.option(1, 0)
        self.assertTrue(self.lib.recover())
        self.assertEqual(self.lib.value(1), 1)
        self.assertEqual(self.lib.value(3), 0)

    def test_uncertain_inflight_blocks_capture_and_restore(self):
        self.lib.option(3, 0)
        self.assertFalse(self.lib.control(1, 3000))
        self.assertEqual(self.lib.value(6), 0)
        self.assertFalse(self.lib.recover())

    def test_metadata_conflict_is_not_overwritten(self):
        self.assertTrue(self.lib.control(1, 3000))
        self.lib.option(2, 0)
        self.assertFalse(self.lib.recover())
        self.assertEqual(self.lib.value(1), 0)
        self.assertEqual(self.lib.value(3), 1)

    def test_failed_write_consumer_notice_uses_baseline_only_renotify(self):
        self.lib.reset(0)
        self.lib.option(0, 1)
        self.assertFalse(self.lib.control(2, 3000))
        self.assertEqual(self.lib.value(1), 0)
        self.assertEqual(self.lib.value(2), 1)
        self.assertEqual(self.lib.value(8), 0)
        self.assertTrue(self.lib.recover())
        self.assertEqual(self.lib.value(8), 1)
        self.assertEqual(self.lib.value(2), 0)
        self.assertEqual(self.lib.value(3), 0)

    def test_existing_backend_requires_explicit_disabled_and_no_pending(self):
        self.assertEqual(self.lib.migration(), 72)
        for requested in (-1, 1):
            self.lib.option(8, requested)
            self.assertEqual(self.lib.migration(), 0)
        self.lib.option(8, 0)
        for pending in (1, 2, 4):
            self.lib.option(9, pending)
            self.assertEqual(self.lib.migration(), 0)

    def test_old_supervisor_lock_prevents_dual_normal_supervision(self):
        self.lib.option(10, 1)
        self.assertEqual(self.lib.migration(), 0)
        self.lib.option(10, 0)
        self.assertEqual(self.lib.migration(), 72)

    def test_fresh_standalone_does_not_require_vd_install(self):
        self.lib.option(7, 0)
        self.assertEqual(self.lib.migration(), 1)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=OUTPUT, help='Directory for generated host fixture and test evidence.')
    OUTPUT = parser.parse_args().output.resolve()
    OUT = OUTPUT / 'checks'
    OUTPUT.mkdir(parents=True, exist_ok=True)
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(StandaloneTests)
    with (OUTPUT / 'tests.log').open('w', encoding='utf-8') as log:
        result = unittest.TextTestRunner(stream=log, verbosity=2).run(suite)
    report = {'passed': result.wasSuccessful(), 'tests': result.testsRun,
              'failures': len(result.failures), 'errors': len(result.errors),
              'coverage': 'real generated C core/serve and LAN/migration policy; fake OS/Binder; real Windows CRT',
              'real_lan_tested': False, 'android_binder_tested': False, 'device_accessed': False}
    (OUTPUT / 'tests.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(report, indent=2))
    if not result.wasSuccessful():
        print((OUTPUT / 'tests.log').read_text(encoding='utf-8'))
        raise SystemExit(1)
