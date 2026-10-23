"""Execute module lifecycle wrappers with a fake helper inside an owned sandbox."""
from pathlib import Path
import subprocess
import tempfile
import unittest

from test_shell_policy import SH

ROOT = Path(__file__).resolve().parents[2]


@unittest.skipUnless(SH, 'Git for Windows sh required')
class MixedVrDeliveryTests(unittest.TestCase):
    def run_fixture(self, script, paired=False, fail=False, mode=2, journal=False, requested=None, token=False):
        with tempfile.TemporaryDirectory(prefix='vd-mixedvr-delivery-', dir=Path(tempfile.gettempdir()) / 'opencode') as tmp:
            root = Path(tmp)
            if paired or token or journal or requested is not None:
                (root / 'state').mkdir()
            (root / 'payload').mkdir()
            (root / 'payload/mode.txt').write_text(f'{mode}\n')
            if paired: (root / 'state/enabled').touch()
            if token: (root / 'state/token').write_text('a'*64+'\n')
            if journal: (root / 'state/baseline').touch()
            if requested is not None: (root / 'state/requested').write_text(f'{requested}\n')
            for name in ('mixedvr.sh', 'coldstart.sh'):
                (root / name).write_text('echo "' + name + ' $1" >> ./calls\n' +
                                        ('exit 1\n' if fail and name == 'mixedvr.sh' else 'exit 0\n'), encoding='utf-8')
            source = (ROOT / 'zygisk' / script).read_text(encoding='utf-8')
            source = source.replace('/data/adb/vdhs_mixedvr', './state')
            source = source.replace('/data/adb/vdhs_zygisk.log', './service.log')
            (root / script).write_text(source, encoding='utf-8')
            runner = ('getprop() { printf 1; }\nsetprop() { :; }\nsleep() { exit 0; }\n. ./service.sh\nwait\n'
                      if script == 'service.sh' else f'sh ./{script}\nstatus=$?\nwait\nexit "$status"\n')
            (root / 'run.sh').write_text(runner, encoding='utf-8')
            result = subprocess.run([SH, './run.sh'], cwd=root, capture_output=True, text=True, timeout=15)
            calls = (root / 'calls').read_text().splitlines() if (root / 'calls').exists() else []
            return result.returncode, calls

    def test_base_new_install_does_not_start_tracking_owner(self):
        self.assertEqual(self.run_fixture('service.sh'), (0, []))

    def test_explicit_disable_survives_boot_even_with_old_enabled_marker(self):
        self.assertEqual(self.run_fixture('service.sh', paired=True, requested=0), (0, ['mixedvr.sh disable']))

    def test_base_upgrade_disables_the_old_enabled_owner(self):
        self.assertEqual(self.run_fixture('service.sh', requested=1, token=True), (0, ['mixedvr.sh disable']))

    def test_legacy_disabled_pairing_without_requested_stays_disabled(self):
        self.assertEqual(self.run_fixture('service.sh', token=True), (0, ['mixedvr.sh disable']))

    def configure_fixture(self, *, pending=False, enabled=False, requested=None, token=None):
        source = (ROOT/'pc/driver/mixedvr.ps1').read_text(encoding='utf-8')
        prefix = "$provision = 'su -c ''"
        start = source.index(prefix)+len(prefix)
        command = source[start:source.index("'''",start)]
        command = command.replace('/data/adb/modules_update/vdhs_zygisk','./pending').replace('/data/adb/modules/vdhs_zygisk','./active').replace('/data/adb/vdhs_mixedvr','./state')
        with tempfile.TemporaryDirectory(prefix='vd-provision-',dir=Path(tempfile.gettempdir())/'opencode') as tmp:
            root=Path(tmp)
            (root/'state').mkdir(); (root/'active').mkdir()
            if pending: (root/'pending').mkdir()
            for name in ('active','pending') if pending else ('active',):
                (root/name/'mixedvr.sh').write_text('echo "'+name+' $1" >> ./calls\n[ -f ./state/token ] || printf "owned-token\\n" > ./state/token\nprintf "1\\n" > ./state/requested\ntouch ./state/enabled\n',encoding='utf-8')
            if enabled: (root/'state/enabled').touch()
            if requested is not None: (root/'state/requested').write_text(f'{requested}\n')
            if token is not None: (root/'state/token').write_text(token)
            result=subprocess.run([SH,'-c',command],cwd=root,capture_output=True,text=True,timeout=15)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            calls=(root/'calls').read_text().splitlines() if (root/'calls').exists() else []
            saved=(root/'state/token').read_text() if (root/'state/token').exists() else None
            return calls,saved

    def test_installer_configures_pending_without_starting_it(self):
        self.assertEqual(self.configure_fixture(pending=True),(['pending enable'],'owned-token\n'))

    def test_connection_reconfiguration_uses_active_and_keeps_token(self):
        self.assertEqual(self.configure_fixture(enabled=True,token='existing\n'),(['active enable'],'existing\n'))

    def test_installer_keeps_explicit_disabled_choice(self):
        self.assertEqual(self.configure_fixture(requested=0,enabled=True,token='existing\n'),([],'existing\n'))

    def test_installer_keeps_legacy_disabled_pairing(self):
        self.assertEqual(self.configure_fixture(token='existing\n'),([],'existing\n'))

    def test_helper_enable_disable_saves_choice_without_replacing_token(self):
        with tempfile.TemporaryDirectory(prefix='vd-mixedvr-choice-', dir=Path(tempfile.gettempdir()) / 'opencode') as tmp:
            root = Path(tmp)
            (root/'state').mkdir(); (root/'bin').mkdir()
            token = 'a'*64+'\n'
            (root/'state/token').write_text(token)
            (root/'bin/tracking-control.jar').write_bytes(b'owned jar fixture')
            helper = root/'bin/vdhs-mixedvrd'
            helper.write_text('#!/bin/sh\nexit 0\n'); helper.chmod(0o755)
            source = (ROOT/'zygisk/mixedvr.sh').read_text(encoding='utf-8').replace('ROOT=/data/adb/vdhs_mixedvr', 'ROOT=./state')
            (root/'mixedvr.sh').write_text(source, encoding='utf-8')
            (root/'run.sh').write_text('''set -e
PATH=/usr/bin:/bin
id() { printf '0\\n'; }
flock() { [ "$#" = 1 ] && [ "$1" = 3 ] && : >&3; }
set -- enable; . ./mixedvr.sh
[ "$(cat ./state/requested)" = 1 ] && [ -f ./state/enabled ]
set -- disable; . ./mixedvr.sh
[ "$(cat ./state/requested)" = 0 ] && [ ! -e ./state/enabled ]
set -- enable; . ./mixedvr.sh
[ "$(cat ./state/requested)" = 1 ] && [ -f ./state/enabled ]
''', encoding='utf-8', newline='\n')
            result = subprocess.run([SH,'./run.sh'],cwd=root,capture_output=True,text=True,timeout=15)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            self.assertEqual((root/'state/token').read_text(),token)

    def test_paired_boot_only_restores_and_disables_the_old_owner(self):
        self.assertEqual(self.run_fixture('service.sh', paired=True), (0, ['mixedvr.sh disable']))

    def test_injection_only_never_starts_paired_companion(self):
        self.assertEqual(self.run_fixture('service.sh', paired=True, mode=1), (0, []))

    def test_injection_only_restores_existing_owned_journal(self):
        self.assertEqual(self.run_fixture('service.sh', paired=True, mode=1, journal=True), (0, ['mixedvr.sh stop']))

    def test_unpaired_uninstall_only_restores_capability(self):
        self.assertEqual(self.run_fixture('uninstall.sh'), (0, ['coldstart.sh disable']))

    def test_paired_uninstall_restores_both_helpers(self):
        self.assertEqual(self.run_fixture('uninstall.sh', paired=True), (0, ['mixedvr.sh disable', 'coldstart.sh disable']))

    def test_failed_mixedvr_restore_still_disarms_capability_and_reports_failure(self):
        self.assertEqual(self.run_fixture('uninstall.sh', paired=True, fail=True), (1, ['mixedvr.sh disable', 'coldstart.sh disable']))


@unittest.skipUnless(SH, 'Git for Windows sh required')
class StandaloneMigrationTests(unittest.TestCase):
    """Check dispatch order with host doubles; Android tests exercise kernel flock."""

    def run_service(self, *, requested='1', enabled=True, marker=None, legacy=True,
                    manager=True, fail=False, changed=None):
        with tempfile.TemporaryDirectory(prefix='vd-migration-', dir=Path(tempfile.gettempdir()) / 'opencode') as tmp:
            root = Path(tmp)
            (root / 'state').mkdir()
            (root / 'module').mkdir()
            if requested is not None:
                (root / 'state/requested').write_text(requested + '\n', encoding='utf-8', newline='\n')
            if enabled:
                (root / 'state/enabled').touch()
            if marker:
                (root / 'module' / marker).touch()
            if legacy:
                (root / 'legacy').mkdir()
            changes = {
                None: '',
                'stop': "printf '0\\n' > ./state/requested\n",
                'disable': 'touch ./module/disable\n',
                'remove': 'touch ./module/remove\n',
                'invalid': "printf 'bad\\n' > ./state/requested\n",
            }
            if manager:
                (root / 'legacy-manager.sh').write_text(
                    'printf "legacy %s begin\\n" "$1" >> ./calls\n' + changes[changed] +
                    ('exit 1\n' if fail else 'printf "legacy done\\n" >> ./calls\n'),
                    encoding='utf-8', newline='\n')
            (root / 'module/standalone.sh').write_text('''printf 'standalone %s\n' "$1" >> ./calls
if [ "$1" = enable ]; then printf '1\n' > ./state/requested; fi
''', encoding='utf-8', newline='\n')
            source = (ROOT / 'knuckles/headset/module/service.sh').read_text(encoding='utf-8')
            source = source.replace('/data/adb/modules/vdhs_zygisk/mixedvr.sh', './legacy-manager.sh')
            source = source.replace('/data/adb/vdhs_mixedvr', './legacy')
            source = source.replace('/data/adb/pico_mixedvr_standalone', './state')
            (root / 'module/service.sh').write_text('''PATH=/usr/bin:/bin
tick=0
getprop() { if [ "$tick" -lt 2 ]; then printf 0; else printf 1; fi; }
sleep() { tick=$((tick + 1)); printf 'boot wait\n' >> ./calls; }
''' + source, encoding='utf-8', newline='\n')
            result = subprocess.run([SH, './module/service.sh'], cwd=root,
                                    capture_output=True, text=True, timeout=15)
            calls = (root / 'calls').read_text().splitlines()
            self.assertEqual(calls[:2], ['boot wait', 'boot wait'])
            saved = root / 'state/requested'
            return result.returncode, calls[2:], saved.read_text().strip() if saved.exists() else None

    def test_enabled_boot_waits_for_successful_legacy_recovery_before_dispatch(self):
        self.assertEqual(self.run_service(),
                         (0, ['legacy disable begin', 'legacy done', 'standalone enable', 'standalone start'], '1'))

    def test_recovery_failure_never_enables_or_starts_the_new_owner(self):
        self.assertEqual(self.run_service(fail=True), (1, ['legacy disable begin'], '1'))

    def test_missing_old_state_or_manager_does_not_add_a_dependency(self):
        for args in ({'legacy': False}, {'manager': False}):
            with self.subTest(args=args):
                self.assertEqual(self.run_service(**args), (0, ['standalone enable', 'standalone start'], '1'))

    def test_explicit_stop_wins_over_an_existing_enabled_marker(self):
        self.assertEqual(self.run_service(requested='0'), (0, [], '0'))

    def test_module_disable_or_remove_skips_migration_and_dispatch(self):
        for marker in ('disable', 'remove'):
            with self.subTest(marker=marker):
                self.assertEqual(self.run_service(marker=marker), (0, [], '1'))

    def test_invalid_saved_choice_is_not_replaced_by_enable(self):
        self.assertEqual(self.run_service(requested='bad'), (1, [], 'bad'))

    def test_unselected_install_does_not_retire_old_state_or_start(self):
        self.assertEqual(self.run_service(requested=None, enabled=False), (0, [], None))

    def test_legacy_opt_in_marker_still_enables_after_recovery(self):
        self.assertEqual(self.run_service(requested=None),
                         (0, ['legacy disable begin', 'legacy done', 'standalone enable', 'standalone start'], '1'))

    def test_stop_saved_during_recovery_is_not_overwritten(self):
        self.assertEqual(self.run_service(changed='stop'), (0, ['legacy disable begin', 'legacy done'], '0'))

    def test_module_disabled_or_removed_during_recovery_does_not_start(self):
        for changed in ('disable', 'remove'):
            with self.subTest(changed=changed):
                self.assertEqual(self.run_service(changed=changed), (0, ['legacy disable begin', 'legacy done'], '1'))

    def test_invalid_choice_after_recovery_is_not_overwritten(self):
        self.assertEqual(self.run_service(changed='invalid'), (1, ['legacy disable begin', 'legacy done'], 'bad'))

    def test_management_lock_failure_leaves_choice_and_journal_untouched(self):
        for action in ('stop', 'disable', 'restore'):
            with self.subTest(action=action), tempfile.TemporaryDirectory(
                    prefix='vd-recovery-lock-', dir=Path(tempfile.gettempdir()) / 'opencode') as tmp:
                root = Path(tmp)
                (root / 'state').mkdir()
                (root / 'state/requested').write_bytes(b'1\n')
                (root / 'state/enabled').touch()
                (root / 'state/baseline').write_bytes(b'owned journal\n')
                (root / 'bin').mkdir()
                helper = root / 'bin/vdhs-mixedvrd'
                helper.write_text('#!/bin/sh\necho restore >> ./calls\n', encoding='utf-8', newline='\n')
                helper.chmod(0o755)
                source = (ROOT / 'zygisk/mixedvr.sh').read_text(encoding='utf-8')
                source = source.replace('/data/adb/vdhs_mixedvr', './state')
                (root / 'mixedvr.sh').write_text('''PATH=/usr/bin:/bin
id() { printf 0; }
flock() { printf 'lock %s\n' "$*" >> ./calls; return 1; }
''' + source, encoding='utf-8', newline='\n')
                result = subprocess.run([SH, './mixedvr.sh', action], cwd=root,
                                        capture_output=True, text=True, timeout=15)
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertEqual((root / 'calls').read_text().splitlines(), ['lock 3'])
                self.assertEqual((root / 'state/requested').read_bytes(), b'1\n')
                self.assertTrue((root / 'state/enabled').is_file())
                self.assertEqual((root / 'state/baseline').read_bytes(), b'owned journal\n')


if __name__ == '__main__': unittest.main(verbosity=2)
