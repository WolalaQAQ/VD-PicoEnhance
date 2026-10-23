"""Build the standalone arm64 daemon and its Magisk package without a device."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import zipfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]


def main():
    sdk = Path(os.environ['ANDROID_SDK_ROOT'])
    jdk = Path(os.environ['JAVA_HOME'])
    llvm = sdk / 'ndk/26.1.10909125/toolchains/llvm/prebuilt/windows-x86_64'
    output = HERE / 'build/module'
    output.mkdir(parents=True, exist_ok=True)
    classes = HERE / 'build/classes'
    classes.mkdir(exist_ok=True)
    binary = output / 'bin/pico-mixedvrd'
    binary.parent.mkdir(exist_ok=True)
    def run(label, arguments):
        process = subprocess.run(list(map(str, arguments)), capture_output=True, timeout=180)
        (HERE / 'build' / (label + '.stdout.log')).write_bytes(process.stdout)
        (HERE / 'build' / (label + '.stderr.log')).write_bytes(process.stderr)
        if process.returncode:
            raise RuntimeError(label + ': ' + process.stderr.decode('utf-8', 'replace'))
    run('clang', [llvm / 'bin/clang.exe', '--target=aarch64-linux-android29',
        '--sysroot=' + str(llvm / 'sysroot'), '-std=c11', '-Wall', '-Wextra', '-Werror',
        '-O2', '-g0', '-Wl,--strip-all', HERE / 'src/mixedvrd.c', '-o', binary])
    android = sdk / 'platforms/android-35/android.jar'
    run('javac', [jdk / 'bin/javac.exe', '-J-Dfile.encoding=UTF-8', '-encoding', 'UTF-8',
        '-source', '8', '-target', '8', '-Xlint:all,-options', '-Werror', '-classpath', android,
        '-d', classes, *sorted((HERE / 'src/java').rglob('*.java'))])
    run('d8', [jdk / 'bin/java.exe', '-Dfile.encoding=UTF-8', '-cp',
        sdk / 'build-tools/35.0.0/lib/d8.jar', 'com.android.tools.r8.D8', '--lib', android,
        '--min-api', '29', '--output', output / 'bin/tracking-control.jar', *sorted(classes.rglob('*.class'))])
    for path in (HERE / 'module').iterdir():
        shutil.copyfile(path, output / path.name)
    shutil.copyfile(HERE / 'src/standalone.sh', output / 'standalone.sh')
    version = next(line.split('=', 1)[1] for line in (ROOT / 'zygisk/module.prop').read_text().splitlines()
                   if line.startswith('version='))
    props = (output / 'module.prop').read_text().replace('version=prototype', 'version=' + version)
    (output / 'module.prop').write_text(props, encoding='utf-8', newline='\n')
    package = HERE.parent / 'dist/PicoHand-Knuckles.zip'
    package.parent.mkdir(exist_ok=True)
    with zipfile.ZipFile(package, 'w', zipfile.ZIP_DEFLATED) as archive:
        for path in sorted(output.rglob('*')):
            if path.is_file():
                info = zipfile.ZipInfo(path.relative_to(output).as_posix())
                mode = 0o755 if path.suffix == '.sh' or path == binary else 0o644
                info.external_attr = (0o100000 | mode) << 16
                info.compress_type = zipfile.ZIP_DEFLATED
                archive.writestr(info, path.read_bytes())
    print(json.dumps(dict(passed=True, package=str(package), device_operations=False)))


if __name__ == '__main__':
    main()
