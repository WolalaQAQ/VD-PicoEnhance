"""Exercise Windows native argv, not a PowerShell function standing in for adb."""
import base64
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest

from test_shell_policy import SH

ROOT = Path(__file__).resolve().parents[2]


@unittest.skipUnless(os.name == 'nt' and SH, 'Windows native argv and Git sh required')
class KnucklesNativeArgumentsTests(unittest.TestCase):
    def check_host(self, host):
        executable = shutil.which(host)
        if not executable:
            self.skipTest(host + ' is not installed')
        with tempfile.TemporaryDirectory(prefix='knuckles native args ', dir=Path(tempfile.gettempdir()) / 'opencode') as temporary:
            work = Path(temporary)
            fake = work / 'native argument recorder.py'
            result = work / 'argv.json'
            fake.write_text('import json,os,sys\nfrom pathlib import Path\nPath(os.environ["KNUCKLES_ARGV_RESULT"]).write_text(json.dumps(sys.argv[1:]))\n', encoding='utf-8')
            # Python is a real native .exe. Its -s flag consumes no value, so the
            # device serial occupies the owned recorder-script position instead.
            command = 'printf "%s %s\\n" "owned one" "owned two"\n'
            def quote(value):
                return "'" + str(value).replace("'", "''") + "'"
            script = work / 'invoke.ps1'
            script.write_text('. ' + quote(ROOT / 'knuckles/manager-core.ps1') + '\n' +
                '$o=@{Adb=' + quote(sys.executable) + ';Serial=' + quote(fake) + '}\n' +
                "Invoke-KnucklesRoot $o @'\n" + command + "'@\n", encoding='ascii')
            env = dict(os.environ, KNUCKLES_ARGV_RESULT=str(result))
            run = subprocess.run([executable, '-NoLogo', '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', script],
                                 env=env, capture_output=True, timeout=30)
            self.assertEqual(run.returncode, 0, run.stderr.decode('utf-8', 'replace'))
            argv = json.loads(result.read_text())
            self.assertEqual(len(argv), 2)
            self.assertEqual(argv[0], 'shell')
            remote = shlex.split(argv[1])
            self.assertEqual(remote[:2], ['su', '-c'])
            self.assertEqual(len(remote), 3)
            encoded = remote[2].split()[1]
            self.assertEqual(base64.b64decode(encoded).decode(), command.rstrip('\n'))
            ran = subprocess.run([SH, '-c', remote[2]], cwd=work, capture_output=True, timeout=10)
            self.assertEqual(ran.returncode, 0, ran.stderr)
            self.assertEqual(ran.stdout.replace(b'\r', b''), b'owned one owned two\n')

    def test_windows_powershell_51(self):
        self.check_host('powershell.exe')

    def test_powershell_7(self):
        self.check_host('pwsh')


@unittest.skipUnless(SH, 'Git sh required')
class KnucklesRetirementTests(unittest.TestCase):
    def run_retirement(self, *, legacy=True, active=False, pending=False, fail_pending=False):
        source = (ROOT / 'knuckles/manager-core.ps1').read_text(encoding='utf-8')
        install = source[source.index('# Stop the old owner before installing its independent successor.'):]
        command = re.search(r"Invoke-KnucklesRoot \$O @'\n(.*?)\n'@", install, re.S).group(1)
        command = command.replace('/data/adb/modules_update/vdhs_zygisk', './pending')
        command = command.replace('/data/adb/modules/vdhs_zygisk', './active')
        command = command.replace('/data/adb/vdhs_mixedvr', './legacy')
        with tempfile.TemporaryDirectory(prefix='knuckles-retire-', dir=Path(tempfile.gettempdir()) / 'opencode') as temporary:
            work = Path(temporary)
            if legacy:
                (work / 'legacy').mkdir()
            for name, exists in (('active', active), ('pending', pending)):
                if exists:
                    (work / name).mkdir()
                    (work / name / 'mixedvr.sh').write_text(
                        'printf "' + name + ' %s\\n" "$1" >> ./calls\n' +
                        ('exit 1\n' if name == 'pending' and fail_pending else 'exit 0\n'),
                        encoding='utf-8', newline='\n')
            done = subprocess.run([SH, '-c', 'PATH=/usr/bin:/bin\n' + command], cwd=work,
                                  capture_output=True, timeout=15)
            calls = (work / 'calls').read_text().splitlines() if (work / 'calls').exists() else []
            return done.returncode, calls

    def test_clean_install_has_no_legacy_dependency(self):
        self.assertEqual(self.run_retirement(legacy=False), (0, []))

    def test_active_manager_still_supports_optional_only_install(self):
        self.assertEqual(self.run_retirement(active=True), (0, ['active disable']))

    def test_staged_manager_handles_upgrade_from_a_base_without_the_script(self):
        self.assertEqual(self.run_retirement(pending=True), (0, ['pending disable']))

    def test_staged_recovery_manager_takes_precedence_over_old_active_code(self):
        self.assertEqual(self.run_retirement(active=True, pending=True), (0, ['pending disable']))

    def test_failed_staged_restore_does_not_fall_back_or_ignore_failure(self):
        self.assertEqual(self.run_retirement(active=True, pending=True, fail_pending=True), (1, ['pending disable']))

    def test_legacy_state_without_either_manager_still_fails(self):
        code, calls = self.run_retirement()
        self.assertNotEqual(code, 0)
        self.assertEqual(calls, [])


if __name__ == '__main__':
    unittest.main()
