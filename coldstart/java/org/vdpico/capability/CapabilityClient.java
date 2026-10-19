package org.vdpico.capability;

import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.net.Credentials;
import android.net.LocalSocket;
import android.net.LocalSocketAddress;
import android.os.Build;
import android.os.IBinder;
import android.os.Process;
import android.system.ErrnoException;
import android.system.Os;
import android.system.OsConstants;

import java.io.IOException;
import java.io.FileInputStream;
import java.io.InputStream;
import java.lang.reflect.Method;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.Callable;
import java.util.concurrent.FutureTask;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;

/** Fixed-purpose app_process client. Exit 3 means no helper socket; exit 2 means failure. */
public final class CapabilityClient {
    private CapabilityClient() { }

    private static final class Result {
        final String line;
        final int exit;

        Result(String line, int exit) {
            this.line = line;
            this.exit = exit;
        }
    }

    public static void main(String[] args) {
        if (Process.myUid() != 0 || Build.VERSION.SDK_INT != 29
                || !CapabilityProtocol.FINGERPRINT.equals(Build.FINGERPRINT)
                || args.length != 1 || !(CapabilityProtocol.isCommand(args[0])
                || "metadata-query".equals(args[0]))) {
            System.err.println("VDC1 client rejected: root, supported firmware and one command required");
            System.exit(2);
            return;
        }
        final String command = args[0];
        FutureTask<Result> task = new FutureTask<Result>(new Callable<Result>() {
            @Override public Result call() throws Exception {
                return "metadata-query".equals(command) ? metadataQuery() : exchange(command);
            }
        });
        Thread worker = new Thread(task, "vdhs-capability-client");
        worker.setDaemon(true);
        worker.start();
        try {
            // Covers connect, request, response and independent Binder metadata query as a whole.
            Result result = task.get(2000, TimeUnit.MILLISECONDS);
            System.out.println(result.line);
            System.exit(result.exit);
        } catch (TimeoutException error) {
            task.cancel(true);
            System.err.println("VDC1 client timeout");
            System.exit(2);
        } catch (Throwable error) {
            // No exception message/stack, snapshots or local paths in client diagnostics.
            System.err.println("VDC1 client protocol or service error");
            System.exit(2);
        }
    }

    private static Result exchange(String command) throws Exception {
        try (LocalSocket socket = new LocalSocket()) {
            try {
                socket.connect(new LocalSocketAddress(CapabilityProtocol.SOCKET,
                        LocalSocketAddress.Namespace.ABSTRACT));
            } catch (IOException error) {
                // Distinguish an absent listener from permission/resource failures.
                Throwable cause = error;
                while (cause != null) {
                    if (cause instanceof ErrnoException) {
                        int errno = ((ErrnoException) cause).errno;
                        if (errno == OsConstants.ECONNREFUSED || errno == OsConstants.ENOENT) {
                            return new Result("VDC1 socket-unavailable reboot-required", 3);
                        }
                        break;
                    }
                    cause = cause.getCause();
                }
                // Android 10 connectLocal uses jniThrowIOException(errno), with no errno cause.
                if (Os.strerror(OsConstants.ECONNREFUSED).equals(error.getMessage())
                        || Os.strerror(OsConstants.ENOENT).equals(error.getMessage())) {
                    return new Result("VDC1 socket-unavailable reboot-required", 3);
                }
                throw error;
            }
            socket.setSoTimeout(2000);
            Credentials peer = socket.getPeerCredentials();
            if (peer.getUid() != 1000 || peer.getPid() <= 0) {
                throw new IOException("Unexpected server peer");
            }
            // A shared system UID alone does not identify system_server.
            try (FileInputStream commandLine = new FileInputStream("/proc/" + peer.getPid() + "/cmdline")) {
                byte[] name = new byte[128];
                int length = commandLine.read(name);
                int end = 0;
                while (end < length && name[end] != 0) ++end;
                if (length <= 0 || end == length || !"system_server".equals(new String(name, 0, end, StandardCharsets.US_ASCII))) {
                    throw new IOException("Peer is not system_server");
                }
            }
            socket.getOutputStream().write(("VDC1 " + command + "\n")
                    .getBytes(StandardCharsets.US_ASCII));
            socket.shutdownOutput();
            InputStream input = socket.getInputStream();
            String response = CapabilityProtocol.readLine(input, 128);
            if (input.read() != -1) {
                throw new IOException("Trailing response data");
            }
            String[] fields = response.split(" ", -1);
            if (fields.length != 7 || !"VDC1".equals(fields[0])
                    || CapabilityProtocol.unsigned(fields[1], Integer.MAX_VALUE) != peer.getPid()
                    || !CapabilityProtocol.oneOf(fields[2], "0", "1")
                    || !CapabilityProtocol.oneOf(fields[3], "waiting", "disabled", "applied",
                    "native", "conflict", "error")
                    || !CapabilityProtocol.oneOf(fields[4], "missing", "int1", "other", "unobserved")
                    || !CapabilityProtocol.oneOf(fields[6], "ok", "error")) {
                throw new IOException("Invalid response fields");
            }
            CapabilityProtocol.unsigned(fields[5], CapabilityProtocol.MAX_GENERATION);
            boolean enabled = "1".equals(fields[2]);
            String phase = fields[3];
            String actual = fields[4];
            boolean success = "ok".equals(fields[6]);
            boolean successfulPhase = "disabled".equals(phase) && !"unobserved".equals(actual)
                    || CapabilityProtocol.oneOf(phase, "applied", "native")
                    && enabled && "int1".equals(actual);
            if ("enable".equals(command) && !enabled
                    || "disable".equals(command) && enabled
                    || "disabled".equals(phase) && enabled
                    || CapabilityProtocol.oneOf(phase, "applied", "native")
                    && (!enabled || !"int1".equals(actual))
                    || success != successfulPhase) {
                throw new IOException("Inconsistent response state");
            }
            return new Result(response, success ? 0 : 2);
        }
    }

    private static Result metadataQuery() throws Exception {
        // This process only uses the ordinary remote IPackageManager API; no PMS private fields.
        Class<?> serviceManager = Class.forName("android.os.ServiceManager");
        IBinder binder = (IBinder) serviceManager.getMethod("checkService", String.class)
                .invoke(null, "package");
        if (binder == null) {
            return new Result("VDC1 metadata-query unobserved error", 2);
        }
        Class<?> stub = Class.forName("android.content.pm.IPackageManager$Stub");
        Object remote = stub.getMethod("asInterface", IBinder.class).invoke(null, binder);
        Class<?> api = Class.forName("android.content.pm.IPackageManager");
        Method getter = api.getMethod("getApplicationInfo", String.class, int.class, int.class);
        ApplicationInfo info = (ApplicationInfo) getter.invoke(remote, CapabilityProtocol.TARGET,
                PackageManager.GET_META_DATA, 0);
        if (info != null && !CapabilityProtocol.TARGET.equals(info.packageName)) {
            throw new IOException("Unexpected query package");
        }
        String actual = CapabilityProtocol.actual(info);
        return new Result("VDC1 metadata-query " + actual + " " + (info == null ? "error" : "ok"),
                info == null ? 2 : 0);
    }
}
