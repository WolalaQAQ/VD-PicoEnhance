package org.vdpico.capability;

import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.net.LocalServerSocket;
import android.net.LocalSocket;
import android.os.Build;
import android.os.Bundle;
import android.os.IBinder;
import android.os.Process;
import android.util.Log;

import java.io.IOException;
import java.io.InputStream;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationHandler;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Map;
import java.util.concurrent.Semaphore;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;

/** PICO Android-10 PMS metadata overlay. No method hooks or persistent writes. */
public final class CapabilityServer {
    private static final String TAG = "vdhs-capability";
    private static final Object START_LOCK = new Object();
    private static Engine instance;

    private CapabilityServer() { }

    /** Called once by the Zygisk system-server callback; never waits for package service. */
    public static void start(boolean enabled) {
        try {
            if (Process.myUid() != 1000 || Build.VERSION.SDK_INT != 29
                    || !CapabilityProtocol.FINGERPRINT.equals(Build.FINGERPRINT)) {
                log("startup rejected: process or firmware mismatch", null);
                return;
            }
            synchronized (START_LOCK) {
                if (instance != null) {
                    return;
                }
                final Engine engine = new Engine(enabled);
                Thread monitor = daemon("vdhs-capability-monitor", new Runnable() {
                    @Override public void run() { engine.monitorLoop(); }
                });
                Thread control = daemon("vdhs-capability-control", new Runnable() {
                    @Override public void run() { engine.controlLoop(); }
                });
                instance = engine;
                control.start();
                monitor.start();
            }
        } catch (Throwable error) {
            log("startup failed", error);
        }
    }

    private static Thread daemon(String name, Runnable task) {
        Thread thread = new Thread(task, name);
        thread.setDaemon(true);
        return thread;
    }

    private static void log(String event, Throwable error) {
        // Exception messages/stacks can contain package metadata or local paths.
        try {
            Log.i(TAG, event + (error == null ? "" : " (" + error.getClass().getSimpleName() + ")"));
        } catch (Throwable ignored) {
            // A logging failure must not escape into SystemServer's uncaught handler.
        }
    }

    private static final class Snapshot {
        final boolean requested;
        final String phase;
        final String actual;
        final long generation;

        Snapshot(boolean requested, String phase, String actual, long generation) {
            this.requested = requested;
            this.phase = phase;
            this.actual = actual;
            this.generation = generation;
        }

        boolean successful() {
            return ("disabled".equals(phase) && !"unobserved".equals(actual))
                    || (("applied".equals(phase) || "native".equals(phase))
                    && requested && "int1".equals(actual));
        }

        String line(boolean validCommand) {
            return "VDC1 " + Process.myPid() + " " + (requested ? "1" : "0")
                    + " " + phase + " " + actual + " " + generation + " "
                    + (validCommand && successful() ? "ok" : "error") + "\n";
        }
    }

    /** A key-only observation made under PMS.mPackages; not a whole-Bundle baseline. */
    private static final class Sample {
        final Object pkg;
        final Bundle metadata;
        final Bundle key;

        Sample(Object pkg, Bundle metadata) {
            this.pkg = pkg;
            this.metadata = metadata;
            key = Engine.singleKey(metadata);
        }
    }

    private static final class Engine {
        // The only lock order is stateLock -> PMS.mPackages. Observers never take stateLock.
        private final Object stateLock = new Object();
        private final Semaphore wake = new Semaphore(0);
        private final AtomicBoolean wakeQueued = new AtomicBoolean(false);
        private boolean requested;
        private String phase = "waiting";
        private String actual = "unobserved";
        private long generation;

        private Object pms;
        private Map<?, ?> packages;
        private Method getter;
        private Field metadataField;
        private Field packageNameField;
        private Object currentPackage;
        private boolean seenPackage;

        private Object ownedPackage;
        private Bundle ownedBundle;
        private Bundle baselineKey;
        private boolean baselinePresent;
        private boolean baselineBundleNull;
        private boolean nativeDeclaration;
        private Bundle nativeBundle;
        // Sticky for this Package incarnation: neither a timer nor enable clears a conflict.
        private boolean conflict;
        private boolean failed;
        private Sample pendingRestore;
        private Bundle pendingRestoreKey;

        private boolean observerAttempted;
        private Object packageList;
        private Object observer;

        Engine(boolean enabled) {
            requested = enabled;
        }

        void signal() {
            if (wakeQueued.compareAndSet(false, true)) {
                wake.release();
            }
        }

        void monitorLoop() {
            for (;;) {
                try {
                    command("status");
                    wake.tryAcquire(1, TimeUnit.SECONDS);
                    wakeQueued.set(false);
                } catch (Throwable error) {
                    log("monitor iteration failed", error);
                    try {
                        Thread.sleep(1000);
                    } catch (Throwable ignored) {
                        // This daemon is process-lifetime; interruption cannot kill SystemServer.
                    }
                }
            }
        }

        Snapshot command(String command) {
            Snapshot result;
            String previous;
            Throwable failure = null;
            synchronized (stateLock) {
                previous = phase;
                if ("enable".equals(command)) {
                    requested = true;
                } else if ("disable".equals(command)) {
                    requested = false;
                }
                try {
                    reconcile("disable".equals(command));
                } catch (Throwable error) {
                    phase = conflict ? "conflict" : "error";
                    actual = "unobserved";
                    failure = error;
                }
                result = new Snapshot(requested, phase, actual, generation);
            }
            // No log/socket I/O inside PMS.mPackages or the state lock.
            if (!previous.equals(result.phase)) {
                log("phase=" + result.phase + " generation=" + result.generation, failure);
            }
            return result;
        }

        private void reconcile(boolean explicitDisable) throws Exception {
            actual = "unobserved";
            if (!bindPackageService()) {
                phase = "waiting";
                return;
            }
            registerObserver();
            // Bound retries when an update races the unlocked normal getter.
            for (int attempt = 0; attempt < 3; ++attempt) {
                Sample sample = sample();
                if (pendingRestore == null && ownedPackage != null && (ownedPackage != sample.pkg
                        || sample.metadata != ownedBundle
                        || !CapabilityProtocol.isIntOne(sample.key))) {
                    conflict = true;
                }
                if (pendingRestore == null && nativeDeclaration && (sample.metadata != nativeBundle
                        || !CapabilityProtocol.isIntOne(sample.key))) {
                    conflict = true;
                }
                // getApplicationInfo can acquire UserManager/permission locks BEFORE its package
                // lock. Always call it with stateLock only, never while holding PMS.mPackages.
                ApplicationInfo before = readApplicationInfo();
                if (!validate(sample, ownedPackage != null || nativeDeclaration || pendingRestore != null)) {
                    if (conflict) {
                        phase = "conflict";
                        return;
                    }
                    continue;
                }
                if (conflict) {
                    phase = "conflict";
                    return;
                }
                if (sample.pkg == null || before == null) {
                    phase = "waiting";
                    return;
                }
                if (pendingRestore != null) {
                    if (sample.pkg != pendingRestore.pkg || sample.metadata != pendingRestore.metadata
                            || !sameKey(pendingRestore.key, sample.metadata)) {
                        conflict = true;
                        phase = "conflict";
                    } else if (!sameKey(pendingRestoreKey, before.metaData)) {
                        failed = true;
                        phase = "error";
                    } else {
                        // Keep the expected original key until a normal query and
                        // stable Package object prove recovery, even across retries.
                        pendingRestore = null;
                        pendingRestoreKey = null;
                        clearOwnership();
                        failed = requested;
                        phase = requested ? "error" : "disabled";
                    }
                    return;
                }
                if (!requested) {
                    if (failed && !explicitDisable) {
                        phase = "error";
                        return;
                    }
                    if (ownedPackage != null) {
                        if (!restoreOwned(sample, false)) {
                            continue;
                        }
                        if ("disabled".equals(phase)) {
                            failed = false;
                        }
                    } else {
                        failed = false;
                        phase = "disabled";
                    }
                    return;
                }
                if (failed) {
                    phase = "error";
                    return;
                }
                if (ownedPackage != null) {
                    if ("int1".equals(actual)) {
                        phase = "applied";
                    } else {
                        if (!restoreOwned(sample, true)) {
                            continue;
                        }
                    }
                    return;
                }
                if (CapabilityProtocol.isIntOne(sample.key)) {
                    if (!"int1".equals(actual)) {
                        failed = true;
                        phase = "error";
                    } else {
                        nativeDeclaration = true;
                        nativeBundle = sample.metadata;
                        phase = "native";
                    }
                    return;
                }
                if (!patch(sample)) {
                    continue;
                }
                return;
            }
            phase = "waiting";
            actual = "unobserved";
        }

        private Sample sample() throws Exception {
            synchronized (packages) {
                Object pkg = packages.get(CapabilityProtocol.TARGET);
                checkPackage(pkg);
                adoptPackage(pkg);
                return new Sample(pkg, pkg == null ? null : (Bundle) metadataField.get(pkg));
            }
        }

        private void checkPackage(Object pkg) throws Exception {
            if (pkg != null && (!"android.content.pm.PackageParser$Package"
                    .equals(pkg.getClass().getName())
                    || !CapabilityProtocol.TARGET.equals(packageNameField.get(pkg)))) {
                throw new IllegalStateException("Unexpected package object");
            }
        }

        /** Called under stateLock, never from a PMS callback. No service/getter/IO in this lock. */
        private boolean validate(Sample sample, boolean sensitive) throws Exception {
            synchronized (packages) {
                return validateLocked(sample, sensitive);
            }
        }

        private boolean validateLocked(Sample sample, boolean sensitive) throws Exception {
            Object pkg = packages.get(CapabilityProtocol.TARGET);
            checkPackage(pkg);
            if (pkg != sample.pkg) {
                adoptPackage(pkg);
                phase = "waiting";
                actual = "unobserved";
                return false;
            }
            Bundle current = pkg == null ? null : (Bundle) metadataField.get(pkg);
            if (current != sample.metadata || !sameKey(sample.key, current)) {
                if (sensitive) {
                    conflict = true;
                    phase = "conflict";
                    actual = "unobserved";
                } else {
                    phase = "waiting";
                    actual = "unobserved";
                }
                return false;
            }
            return true;
        }

        private boolean bindPackageService() throws Exception {
            if (pms != null) {
                return true;
            }
            Class<?> serviceManager = Class.forName("android.os.ServiceManager");
            IBinder binder = (IBinder) serviceManager.getMethod("checkService", String.class)
                    .invoke(null, "package");
            if (binder == null) {
                return false;
            }
            Object local = binder.queryLocalInterface(CapabilityProtocol.DESCRIPTOR);
            if (local == null || !"com.android.server.pm.PackageManagerService"
                    .equals(local.getClass().getName())) {
                throw new IllegalStateException("Package service is not local PMS");
            }
            Field mapField = local.getClass().getDeclaredField("mPackages");
            mapField.setAccessible(true);
            Object map = mapField.get(local);
            if (!(map instanceof Map) || !"android.util.ArrayMap".equals(map.getClass().getName())) {
                throw new IllegalStateException("Unexpected PMS lock/map");
            }
            Class<?> pkgClass = Class.forName("android.content.pm.PackageParser$Package",
                    false, local.getClass().getClassLoader());
            Field data = pkgClass.getField("mAppMetaData");
            Field name = pkgClass.getField("packageName");
            if (data.getType() != Bundle.class || name.getType() != String.class) {
                throw new IllegalStateException("Unexpected PMS field types");
            }
            data.setAccessible(true);
            name.setAccessible(true);
            Method read = local.getClass().getMethod("getApplicationInfo",
                    String.class, int.class, int.class);
            read.setAccessible(true);
            // Publish bindings only after every necessary member was resolved.
            packages = (Map<?, ?>) map;
            getter = read;
            metadataField = data;
            packageNameField = name;
            pms = local;
            return true;
        }

        private void registerObserver() {
            if (observerAttempted) {
                return;
            }
            observerAttempted = true;
            try {
                ClassLoader loader = pms.getClass().getClassLoader();
                Class<?> internalClass = Class.forName("android.content.pm.PackageManagerInternal",
                        false, loader);
                Class<?> observerClass = Class.forName(
                        "android.content.pm.PackageManagerInternal$PackageListObserver", false, loader);
                Class<?> localServices = Class.forName("com.android.server.LocalServices", false, loader);
                Object internal = localServices.getMethod("getService", Class.class)
                        .invoke(null, internalClass);
                if (internal == null) {
                    return; // The mandatory one-second scan remains available.
                }
                observer = Proxy.newProxyInstance(loader, new Class<?>[] {observerClass},
                        new InvocationHandler() {
                            @Override public Object invoke(Object proxy, Method method, Object[] args) {
                                try {
                                    String name = method.getName();
                                    if ("hashCode".equals(name)) {
                                        return Integer.valueOf(System.identityHashCode(proxy));
                                    }
                                    if ("equals".equals(name)) {
                                        return Boolean.valueOf(args != null && args.length == 1
                                                && proxy == args[0]);
                                    }
                                    if ("toString".equals(name)) {
                                        return "VdCapabilityObserver";
                                    }
                                    if (CapabilityProtocol.oneOf(name, "onPackageAdded",
                                            "onPackageChanged", "onPackageRemoved")
                                            && args != null && args.length == 2
                                            && CapabilityProtocol.TARGET.equals(args[0])) {
                                        signal();
                                    }
                                } catch (Throwable ignored) {
                                    // No PMS reflection, state lock or I/O on a notification stack.
                                }
                                return null;
                            }
                        });
                packageList = internalClass.getMethod("getPackageList", observerClass)
                        .invoke(internal, observer);
            } catch (Throwable ignored) {
                // Optional observer failure does not weaken ownership or enable broad exemptions.
            }
        }

        private void adoptPackage(Object pkg) {
            if (seenPackage && pkg == currentPackage) {
                return;
            }
            seenPackage = true;
            if (pkg != currentPackage) {
                if (generation == CapabilityProtocol.MAX_GENERATION) {
                    throw new IllegalStateException("Generation exhausted");
                }
                ++generation;
            }
            currentPackage = pkg;
            clearOwnership();
            pendingRestore = null;
            pendingRestoreKey = null;
            nativeDeclaration = false;
            nativeBundle = null;
            conflict = false;
            failed = false;
        }

        private ApplicationInfo readApplicationInfo() throws Exception {
            ApplicationInfo info = (ApplicationInfo) getter.invoke(pms,
                    CapabilityProtocol.TARGET, PackageManager.GET_META_DATA, 0);
            if (info != null && !CapabilityProtocol.TARGET.equals(info.packageName)) {
                throw new IllegalStateException("Unexpected getter package");
            }
            actual = CapabilityProtocol.actual(info);
            return info;
        }

        private static Bundle singleKey(Bundle current) {
            Bundle result = current == null ? new Bundle() : new Bundle(current);
            for (String key : new ArrayList<String>(result.keySet())) {
                if (!CapabilityProtocol.KEY.equals(key)) {
                    result.remove(key);
                }
            }
            return result;
        }

        private boolean patch(Sample original) throws Exception {
            Sample patched;
            try {
                synchronized (packages) {
                    if (!validateLocked(original, true)) {
                        return conflict;
                    }
                    Bundle current = (Bundle) metadataField.get(original.pkg);
                    Bundle replacement = current == null ? new Bundle() : new Bundle(current);
                    replacement.putInt(CapabilityProtocol.KEY, 1);
                    // Build the readback sample before publishing, so an allocation failure cannot
                    // leave a write without its Package/key ownership record.
                    patched = new Sample(original.pkg, replacement);
                    metadataField.set(original.pkg, replacement);
                    baselinePresent = original.key.containsKey(CapabilityProtocol.KEY);
                    baselineKey = original.key;
                    baselineBundleNull = original.metadata == null;
                    ownedPackage = original.pkg;
                    ownedBundle = replacement;
                }
            } catch (Throwable ignored) {
                failed = true;
                phase = "error";
                return true;
            }
            try {
                ApplicationInfo info = readApplicationInfo();
                if (!validate(patched, true)) {
                    return conflict;
                }
                if (info == null || !"int1".equals(actual)) {
                    throw new IllegalStateException("Patch readback failed");
                }
                phase = "applied";
                return true;
            } catch (Throwable ignored) {
                return restoreOwned(patched, true);
            }
        }

        private boolean restoreOwned(Sample original, boolean compensation) throws Exception {
            Bundle saved = baselineKey;
            boolean present = baselinePresent;
            boolean wasNull = baselineBundleNull;
            Sample restoredSample;
            failed = compensation;
            try {
                synchronized (packages) {
                    if (!validateLocked(original, true)) {
                        return conflict;
                    }
                    Bundle current = (Bundle) metadataField.get(original.pkg);
                    if (original.pkg != ownedPackage || current != ownedBundle
                            || !CapabilityProtocol.isIntOne(current)) {
                        conflict = true;
                        phase = "conflict";
                        return true;
                    }
                    Bundle restored = new Bundle(current);
                    restored.remove(CapabilityProtocol.KEY);
                    if (present) {
                        restored.putAll(saved);
                    }
                    if (wasNull && restored.isEmpty()) {
                        restored = null;
                    }
                    restoredSample = new Sample(original.pkg, restored);
                    metadataField.set(original.pkg, restored);
                    pendingRestore = restoredSample;
                    pendingRestoreKey = saved;
                    clearOwnership();
                }
                // Normal PMS getter is deliberately outside the map lock.
                ApplicationInfo info = readApplicationInfo();
                if (!validate(restoredSample, true)) {
                    return conflict;
                }
                Bundle readback = info == null ? null : info.metaData;
                if (info == null || !sameKey(saved, readback)
                        || present != (readback != null && readback.containsKey(CapabilityProtocol.KEY))) {
                    failed = true;
                    phase = "error";
                } else if (compensation) {
                    pendingRestore = null;
                    pendingRestoreKey = null;
                    failed = true;
                    phase = "error";
                } else {
                    pendingRestore = null;
                    pendingRestoreKey = null;
                    failed = false;
                    phase = "disabled";
                }
            } catch (Throwable error) {
                failed = true;
                phase = conflict ? "conflict" : "error";
                actual = "unobserved";
            }
            return true;
        }

        @SuppressWarnings("deprecation") // Untyped reads preserve API-29 Bundle key type semantics.
        private static boolean sameKey(Bundle original, Bundle current) {
            boolean present = original != null && original.containsKey(CapabilityProtocol.KEY);
            boolean currentPresent = current != null && current.containsKey(CapabilityProtocol.KEY);
            if (present != currentPresent || !present) {
                return present == currentPresent;
            }
            Object first = original.get(CapabilityProtocol.KEY);
            Object second = current.get(CapabilityProtocol.KEY);
            if (first == second) {
                return true;
            }
            if (first == null || second == null || first.getClass() != second.getClass()) {
                return false;
            }
            // Manifest metadata uses immutable scalar types. Never call an arbitrary Parcelable's
            // equals method on a PMS lock stack; opaque values must retain object identity.
            Class<?> type = first.getClass();
            return (type == String.class || type == Integer.class || type == Boolean.class
                    || type == Float.class || type == Long.class || type == Double.class
                    || type == Byte.class || type == Short.class || type == Character.class)
                    && first.equals(second);
        }

        private void clearOwnership() {
            ownedPackage = null;
            ownedBundle = null;
            baselineKey = null;
            baselinePresent = false;
            baselineBundleNull = false;
        }

        void controlLoop() {
            for (;;) {
                try (LocalServerSocket server = new LocalServerSocket(CapabilityProtocol.SOCKET)) {
                    for (;;) {
                        LocalSocket accepted = server.accept();
                        try (LocalSocket peer = accepted) {
                            peer.setSoTimeout(1000);
                            if (peer.getPeerCredentials().getUid() != 0) {
                                continue;
                            }
                            InputStream input = peer.getInputStream();
                            String line = CapabilityProtocol.readLine(input, 32);
                            // One request per connection; clients half-close their write side.
                            if (input.read() != -1) {
                                throw new IOException("Trailing request data");
                            }
                            boolean valid = line.startsWith("VDC1 ")
                                    && CapabilityProtocol.isCommand(line.substring(5));
                            String command = valid ? line.substring(5) : "status";
                            Snapshot snapshot = command(command);
                            peer.getOutputStream().write(snapshot.line(valid)
                                    .getBytes(StandardCharsets.US_ASCII));
                        } catch (Throwable error) {
                            log("control request failed", error);
                        }
                    }
                } catch (Throwable error) {
                    log("control listener unavailable", error);
                    try {
                        Thread.sleep(5000);
                    } catch (Throwable ignored) {
                        // Retry only the fixed abstract listener, never a filesystem path.
                    }
                }
            }
        }
    }
}
