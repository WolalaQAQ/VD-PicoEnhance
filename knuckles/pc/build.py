"""Build own Windows driver/fixtures only; never loads or registers SteamVR."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import pefile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
WORKSPACE = ROOT.parent
ZIG = Path(os.environ['ZIG'])
SDK = Path(os.environ['OPENVR_ROOT'])
CORE = ROOT / 'pc/driver/src'
CAPTURE = HERE.parent / 'capture'
SOURCES = ['input_probe.c', 'sdk_sampler.cpp', 'worker.cpp', 'switch_policy.c',
           'hand_preference.cpp', 'headset_client.c', 'phase_control.c']

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--core-only', action='store_true')
    args = parser.parse_args()
    out = HERE / 'build' / datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
    out.mkdir(parents=True)
    env = dict(os.environ)
    env['ZIG_GLOBAL_CACHE_DIR'] = str(HERE / 'build/zig-global-cache')
    env['ZIG_LOCAL_CACHE_DIR'] = str(HERE / 'build/zig-local-cache')
    env['TEMP'] = env['TMP'] = str(HERE / 'build/tmp')
    for key in ('ZIG_GLOBAL_CACHE_DIR', 'ZIG_LOCAL_CACHE_DIR', 'TEMP'): Path(env[key]).mkdir(exist_ok=True)
    commands = []
    def run(label, argv):
        p = subprocess.run(list(map(str, argv)), env=env, capture_output=True, timeout=300)
        (out / f'{label}.stdout.log').write_bytes(p.stdout)
        (out / f'{label}.stderr.log').write_bytes(p.stderr)
        commands.append(dict(label=label, command=list(map(str, argv)), returncode=p.returncode))
        (out / 'commands.json').write_text(json.dumps(commands, indent=2), encoding='utf-8')
        if p.returncode: raise RuntimeError(f'{label}: {p.stderr.decode("utf-8", "replace")}')
        return p.stdout
    common = ['-target', 'x86_64-windows-gnu', '-O2', '-Wall', '-Wextra', '-Werror',
              '-I', CORE, '-I', HERE / 'src',
              '-isystem', SDK / 'headers', '-I', CAPTURE]
    objects = []
    inventory = {}
    for path in [CORE / name for name in SOURCES] + [HERE / 'src/controller.cpp']:
        target = out / (path.name + '.o')
        cpp = path.suffix == '.cpp'
        run(path.stem, [ZIG, 'c++' if cpp else 'cc', *common, '-std=c++17' if cpp else '-std=c11',
                       '-c', path, '-o', target])
        inventory[str(path.relative_to(ROOT))] = hashlib.sha256(path.read_bytes()).hexdigest()
        objects.append(target)
    fixture = out / 'owned-controller.exe'
    run('link-fixture', [ZIG, 'c++', *common, '-std=c++17', HERE / 'checks/controller_fixture.cpp', *objects, '-o', fixture])
    stdout = run('run-fixture', [fixture])
    fixture_result = json.loads(stdout.decode().strip().splitlines()[-1])
    result = dict(passed=True, controller=fixture_result, output=str(out),
                  device_operations=False, runtime_loaded=False, registered=False)
    lan_fixture = out / 'owned-lan.exe'
    run('link-lan', [ZIG, 'c++', *common, '-std=c++17', HERE / 'checks/lan_fixture.cpp',
        HERE / 'src/lan_transport.cpp', out / 'headset_client.c.o', '-lws2_32', '-lbcrypt', '-liphlpapi', '-o', lan_fixture])
    run('run-lan', [sys.executable, '-B', '-X', 'utf8', HERE / 'test_lan.py', lan_fixture, '--output', out])
    result['lan'] = json.loads((out / 'lan-results.json').read_text())
    run('run-discovery', [sys.executable, '-B', '-X', 'utf8', HERE / 'test_lan.py', lan_fixture,
        '--scenario', 'discovery', '--output', out / 'discovery-test'])
    result['discovery'] = json.loads((out / 'discovery-test/discovery-results.json').read_text())
    run('run-expired-discovery', [sys.executable, '-B', '-X', 'utf8', HERE / 'test_lan.py', lan_fixture,
        '--scenario', 'expired-discovery', '--output', out / 'expired-discovery-test'])
    result['expired_discovery'] = json.loads((out / 'expired-discovery-test/discovery-results.json').read_text())
    if not args.core_only:
        target = out / 'driver_picomixedvr/bin/win64/driver_picomixedvr.dll'
        target.parent.mkdir(parents=True)
        run('link-driver', [ZIG, 'c++', *common, '-std=c++17', '-g0', '-s', '-shared',
            HERE / 'src/provider.cpp', HERE / 'src/lan_transport.cpp', CAPTURE / 'input_capture.cpp',
            *objects, '-lws2_32', '-lbcrypt', '-liphlpapi', '-o', target])
        shutil.copyfile(HERE / 'driver.vrdrivermanifest', out / 'driver_picomixedvr/driver.vrdrivermanifest')
        shutil.copyfile(SDK / 'LICENSE', out / 'driver_picomixedvr/OPENVR-LICENSE.txt')
        notices = []
        for name in ('LICENSE', 'lib/libcxx/LICENSE.TXT', 'lib/libcxxabi/LICENSE.TXT',
                     'lib/libunwind/LICENSE.TXT', 'lib/libc/mingw/COPYING'):
            notices.append('\n=== Zig 0.13.0: ' + name + ' ===\n' + (ZIG.parent / name).read_text(encoding='utf-8'))
        (out / 'driver_picomixedvr/ZIG-RUNTIME-NOTICES.txt').write_text('\n'.join(notices), encoding='utf-8')
        result['driver'] = str(target)
        provider_fixture = out / 'owned-provider.exe'
        run('link-provider-fixture', [ZIG, 'c++', *common, '-std=c++17', HERE / 'checks/provider_fixture.cpp',
            *objects, '-o', provider_fixture])
        run('run-provider-fixture', [sys.executable, '-B', '-X', 'utf8', HERE / 'test_lan.py',
            provider_fixture, '--driver', target, '--output', out / 'provider-test'])
        result['provider'] = json.loads((out / 'provider-test/provider-results.json').read_text())
        result['provider_discovery'] = {}
        for scenario in ('discovery', 'reconnect', 'stop-discovery', 'helper-error', 'bad-ack'):
            directory = out / ('provider-' + scenario)
            run('run-provider-' + scenario, [sys.executable, '-B', '-X', 'utf8', HERE / 'test_lan.py',
                provider_fixture, '--driver', target, '--scenario', scenario, '--output', directory])
            result['provider_discovery'][scenario] = json.loads((directory / 'discovery-results.json').read_text())
        image = pefile.PE(data=target.read_bytes())
        exported = sorted(s.name.decode() for s in image.DIRECTORY_ENTRY_EXPORT.symbols if s.name)
        imports = sorted(item.dll.decode().lower() for item in image.DIRECTORY_ENTRY_IMPORT)
        # Zig's linked Windows unwind runtime also carries dllexport symbols.
        # The required provider factory, not the total export count, is the ABI.
        assert 'HmdDriverFactory' in exported, exported
        assert image.FILE_HEADER.Machine == 0x8664 and image.FILE_HEADER.Characteristics & 0x2000
        assert not any('virtualdesktop' in name or 'openvr' in name for name in imports)
        result['pe'] = dict(exports=exported, imports=imports, driver_sha256=hashlib.sha256(target.read_bytes()).hexdigest())
    snapshot_paths = {CORE / name for name in SOURCES}
    snapshot_paths |= {p.with_suffix('.h') for p in snapshot_paths if p.with_suffix('.h').exists()}
    snapshot_paths |= set((HERE / 'src').glob('*')) | set((HERE / 'checks').glob('*.cpp'))
    snapshot_paths.add(HERE.parent / 'headset/src/lan_config.h')
    if not args.core_only:
        snapshot_paths |= {CAPTURE / 'input_capture.cpp', CAPTURE / 'input_capture.h'}
    for path in snapshot_paths:
        relative = path.relative_to(ROOT)
        destination = out / 'source-snapshot' / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(path, destination)
        inventory[str(relative)] = hashlib.sha256(path.read_bytes()).hexdigest()
    (out / 'source-inputs.json').write_text(json.dumps(inventory, indent=2), encoding='utf-8')
    (out / 'results.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
    print(json.dumps(result, indent=2))

if __name__ == '__main__': main()
