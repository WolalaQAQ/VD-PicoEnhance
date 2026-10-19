"""Exercise the production VD-only PMS overlay with small Android host doubles."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
JDK = Path(os.environ['JAVA_HOME']) if os.environ.get('JAVA_HOME') else None
JAVAC = str(JDK / 'bin/javac.exe') if JDK else shutil.which('javac')
JAVA = str(JDK / 'bin/java.exe') if JDK else shutil.which('java')
SDK = Path(os.environ['ANDROID_SDK_ROOT']) if os.environ.get('ANDROID_SDK_ROOT') else None

STUBS = {
    'android/os/Bundle.java': '''package android.os;
public class Bundle {
    private final java.util.Map<String,Object> values = new java.util.HashMap<String,Object>();
    public Bundle() { }
    public Bundle(Bundle source) { values.putAll(source.values); }
    public boolean containsKey(String key) { return values.containsKey(key); }
    public Object get(String key) { return values.get(key); }
    public java.util.Set<String> keySet() { return values.keySet(); }
    public void putInt(String key, int value) { values.put(key, Integer.valueOf(value)); }
    public void putString(String key, String value) { values.put(key, value); }
    public void putAll(Bundle source) { values.putAll(source.values); }
    public void remove(String key) { values.remove(key); }
    public boolean isEmpty() { return values.isEmpty(); }
}''',
    'android/os/Process.java': '''package android.os;
public class Process { public static int myPid() { return 123; } public static int myUid() { return 1000; } }''',
    'android/os/IBinder.java': '''package android.os;
public interface IBinder { Object queryLocalInterface(String descriptor); }''',
    'android/os/ServiceManager.java': '''package android.os;
public class ServiceManager {
    public static Object current;
    public static IBinder checkService(String name) {
        if (!"package".equals(name) || current == null) return null;
        return new IBinder() { public Object queryLocalInterface(String descriptor) { return current; } };
    }
}''',
    'android/util/ArrayMap.java': '''package android.util;
public class ArrayMap<K,V> extends java.util.HashMap<K,V> { private static final long serialVersionUID = 1L; }''',
    'android/util/Log.java': '''package android.util;
public class Log { public static int i(String tag, String text) { return 0; } }''',
    'android/content/pm/PackageManager.java': '''package android.content.pm;
public class PackageManager { public static final int GET_META_DATA = 128; }''',
    'android/content/pm/ApplicationInfo.java': '''package android.content.pm;
public class ApplicationInfo { public String packageName; public android.os.Bundle metaData; }''',
    'android/content/pm/PackageParser.java': '''package android.content.pm;
public class PackageParser {
    public static class Package { public String packageName; public android.os.Bundle mAppMetaData; }
}''',
    'com/android/server/pm/PackageManagerService.java': '''package com.android.server.pm;
public class PackageManagerService {
    public final android.util.ArrayMap<String,android.content.pm.PackageParser.Package> mPackages =
        new android.util.ArrayMap<String,android.content.pm.PackageParser.Package>();
    public int reads, failRead;
    public android.content.pm.ApplicationInfo getApplicationInfo(String name, int flags, int user) {
        if (++reads == failRead) throw new IllegalStateException("test read failure");
        android.content.pm.PackageParser.Package pkg = mPackages.get(name);
        if (pkg == null) return null;
        android.content.pm.ApplicationInfo info = new android.content.pm.ApplicationInfo();
        info.packageName = pkg.packageName;
        info.metaData = pkg.mAppMetaData == null ? null : new android.os.Bundle(pkg.mAppMetaData);
        return info;
    }
}''',
}

HARNESS = r'''import android.os.Bundle;
import android.content.pm.PackageParser.Package;
import com.android.server.pm.PackageManagerService;
import java.lang.reflect.*;

public class CapabilityHarness {
    static final String TARGET = "VirtualDesktop.Android", KEY = "handtracking";
    static final Class<?> ENGINE;
    static final Constructor<?> NEW;
    static final Method COMMAND;
    static {
        try {
            ENGINE = Class.forName("org.vdpico.capability.CapabilityServer$Engine");
            NEW = ENGINE.getDeclaredConstructor(boolean.class); NEW.setAccessible(true);
            COMMAND = ENGINE.getDeclaredMethod("command", String.class); COMMAND.setAccessible(true);
        } catch (Exception error) { throw new RuntimeException(error); }
    }
    static void check(boolean ok, String message) { if (!ok) throw new AssertionError(message); }
    static Object field(Object value, String name) throws Exception {
        Field field = value.getClass().getDeclaredField(name); field.setAccessible(true); return field.get(value);
    }
    static Package pkg(Bundle metadata) {
        Package value = new Package(); value.packageName = TARGET; value.mAppMetaData = metadata; return value;
    }
    static Object command(Object engine, String command, String expected) throws Exception {
        Object value = COMMAND.invoke(engine, command);
        check(expected.equals(field(value, "phase")), "unexpected phase: " + field(value, "phase"));
        return value;
    }
    public static void main(String[] args) throws Exception {
        String scenario = args[0];
        PackageManagerService pms = new PackageManagerService(); android.os.ServiceManager.current = pms;
        Bundle baseline = scenario.equals("null") ? null : new Bundle();
        if (scenario.equals("string")) baseline.putString(KEY, "1");
        if (scenario.equals("native")) baseline.putInt(KEY, 1);
        if (baseline != null) baseline.putString("other", "original");
        Package target = pkg(baseline); pms.mPackages.put(TARGET, target);
        Package other = new Package(); other.packageName = "com.example.other"; other.mAppMetaData = new Bundle();
        pms.mPackages.put(other.packageName, other); Bundle otherBundle = other.mAppMetaData;
        Object engine = NEW.newInstance(!scenario.equals("disabled"));
        if (scenario.equals("waiting")) {
            android.os.ServiceManager.current = null; command(engine, "status", "waiting");
            check(target.mAppMetaData == baseline, "waiting changed metadata");
            android.os.ServiceManager.current = pms;
        }
        if (scenario.equals("read-failure")) {
            pms.failRead = 2;
            command(engine, "status", "error");
            check(target.mAppMetaData != null && !target.mAppMetaData.containsKey(KEY), "failed readback left declaration behind");
            return;
        }
        if (scenario.equals("disabled")) {
            command(engine, "status", "disabled");
            check(target.mAppMetaData == baseline, "disabled helper changed metadata");
        } else {
            Object first = command(engine, "status", scenario.equals("native") ? "native" : "applied");
            check("int1".equals(field(first, "actual")), "normal getter did not confirm int1");
            check(target.mAppMetaData.get(KEY) instanceof Integer, "declaration must be an integer");
            if (scenario.equals("conflict")) {
                target.mAppMetaData = new Bundle(target.mAppMetaData);
                target.mAppMetaData.putInt(KEY, 0);
                Bundle external = target.mAppMetaData;
                command(engine, "status", "conflict"); command(engine, "enable", "conflict");
                command(engine, "disable", "conflict");
                check(target.mAppMetaData == external && ((Integer)external.get(KEY)).intValue() == 0,
                    "overwrote external ownership");
                return;
            }
            if (scenario.equals("update")) {
                long generation = ((Long)field(first, "generation")).longValue();
                Bundle replacement = new Bundle(); replacement.putString(KEY, "updated-baseline");
                target = pkg(replacement); pms.mPackages.put(TARGET, target);
                Object updated = command(engine, "status", "applied");
                check(((Long)field(updated, "generation")).longValue() > generation, "new package did not advance generation");
            }
            if (scenario.equals("other-key")) target.mAppMetaData.putString("later", "keep");
            command(engine, "disable", "disabled");
            if (scenario.equals("native")) check(((Integer)target.mAppMetaData.get(KEY)).intValue() == 1, "removed native declaration");
            else if (scenario.equals("string")) check("1".equals(target.mAppMetaData.get(KEY)), "lost original type/value");
            else if (scenario.equals("update")) check("updated-baseline".equals(target.mAppMetaData.get(KEY)), "restored previous package baseline");
            else if (scenario.equals("null")) check(target.mAppMetaData == null, "null Bundle was not restored");
            else check(!target.mAppMetaData.containsKey(KEY), "declaration was not removed");
            if (scenario.equals("other-key")) check("keep".equals(target.mAppMetaData.get("later")), "lost another key's later update");
        }
        check(other.mAppMetaData == otherBundle && !otherBundle.containsKey(KEY), "modified another package");
        System.out.println("PASS " + scenario);
    }
}'''


@unittest.skipUnless(JAVAC and JAVA and SDK, 'JAVA_HOME and ANDROID_SDK_ROOT required')
class CapabilityOverlayTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        parent = Path(tempfile.gettempdir()) / 'opencode'
        parent.mkdir(exist_ok=True)
        cls.temp = tempfile.TemporaryDirectory(prefix='vd-capability-', dir=parent)
        cls.addClassCleanup(cls.temp.cleanup)
        output = Path(cls.temp.name)
        cls.classes = output / 'classes'
        cls.classes.mkdir()
        android = SDK / 'platforms/android-35/android.jar'
        sources = []
        for name, text in {**STUBS, 'CapabilityHarness.java': HARNESS}.items():
            path = output / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding='utf-8')
            sources.append(path)
        production = ROOT / 'coldstart/java/org/vdpico/capability'
        sources += [production / name for name in ['CapabilityProtocol.java', 'CapabilityServer.java']]
        result = subprocess.run([JAVAC, '-J-Dfile.encoding=UTF-8', '-encoding', 'UTF-8',
                                 '-source', '8', '-target', '8', '-Xlint:all,-options,-deprecation', '-Werror',
                                 '-classpath', str(android), '-d', str(cls.classes), *map(str, sources)],
                                capture_output=True, text=True, encoding='utf-8', timeout=60)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)
        cls.classpath = str(cls.classes) + os.pathsep + str(android)

    def run_scenario(self, scenario):
        result = subprocess.run([JAVA, '-Dfile.encoding=UTF-8', '-cp', self.classpath, 'CapabilityHarness', scenario],
                                capture_output=True, text=True, encoding='utf-8', timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_enable_and_restore_only_the_target_package(self): self.run_scenario('normal')
    def test_restore_original_null_metadata(self): self.run_scenario('null')
    def test_restore_original_key_type_and_value(self): self.run_scenario('string')
    def test_keep_native_integer_declaration(self): self.run_scenario('native')
    def test_keep_later_changes_to_other_keys(self): self.run_scenario('other-key')
    def test_external_conflict_is_not_overwritten(self): self.run_scenario('conflict')
    def test_package_update_gets_its_own_baseline(self): self.run_scenario('update')
    def test_wait_for_package_service_before_modifying(self): self.run_scenario('waiting')
    def test_disabled_helper_does_not_modify_metadata(self): self.run_scenario('disabled')
    def test_failed_readback_compensates_the_declaration(self): self.run_scenario('read-failure')


class CapabilityIntegrationTests(unittest.TestCase):
    def test_uses_server_callback_without_loading_vd_runtime_into_system_server(self):
        source = (ROOT / 'zygisk/jni/module.cpp').read_text()
        start = source.index('void preServerSpecialize(')
        server = source[start:source.index('void preAppSpecialize(', start)]
        self.assertIn('vd_capability_prepare(directory)', server)
        self.assertIn('vd_capability_start(env_)', server)
        self.assertNotIn('dlopen', server)
        native = (ROOT / 'zygisk/jni/vd_capability.cpp').read_text()
        self.assertIn('vdhs_config_read(stream, 2)', native)
        self.assertIn('requested = true;', native)

    def test_build_and_recovery_do_not_include_mixedvr(self):
        build = (ROOT / 'zygisk/build.ps1').read_text()
        self.assertIn("'coldstart'", build)
        self.assertIn("'capability'", build)
        self.assertNotIn('mixedvr', build.lower())
        self.assertNotIn('mixedvr.sh', (ROOT / 'zygisk/uninstall.sh').read_text())
        source = (ROOT / 'zygisk/coldstart.sh').read_text()
        self.assertNotIn('setprop', source)
        self.assertNotIn('sys_tracking_hand_enable', source)


if __name__ == '__main__':
    unittest.main()
