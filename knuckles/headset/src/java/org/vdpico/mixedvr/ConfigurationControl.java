package org.vdpico.mixedvr;

import android.os.Build;
import android.os.IBinder;
import android.os.Parcel;
import android.os.Process;
import android.system.Os;
import android.system.OsConstants;
import android.system.StructStat;
import java.io.FileDescriptor;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.lang.reflect.Method;
import java.nio.charset.StandardCharsets;
import java.util.HashSet;
import java.util.Iterator;
import java.util.Set;
import java.util.UUID;
import org.json.JSONArray;
import org.json.JSONObject;

/** Current-firmware fixed-key configuration bridge. No general-purpose setter. */
public final class ConfigurationControl {
    private static boolean uncertainWrite;
    public static boolean hasUncertainWrite() { return uncertainWrite; }
    private static final String ROOT = "/data/adb/pico_mixedvr_standalone";
    private static final String SNAPSHOT = ROOT + "/configuration.json";
    private static final String KEY = "sys_tracking_hand_enable";
    private static final String DESCRIPTOR = "com.pvr.configuration.IConfigServiceInterface";
    private static final String FIRMWARE = "Pico/Phoenix/PICOA8110:10/5.13.7/smartcm.1761755159:user/dev-keys";

    private static IBinder service() throws Exception {
        if (!FIRMWARE.equals(Build.FINGERPRINT)) throw new IllegalStateException("unsupported firmware");
        Method method = Class.forName("android.os.ServiceManager").getDeclaredMethod("checkService", String.class);
        IBinder binder = (IBinder)method.invoke(null, "ConfigurationService");
        if (binder == null || !DESCRIPTOR.equals(binder.getInterfaceDescriptor())) {
            throw new IllegalStateException("configuration service unavailable");
        }
        return binder;
    }

    private static String get(IBinder binder, int type, String name) throws Exception {
        Parcel input = Parcel.obtain();
        Parcel output = Parcel.obtain();
        try {
            input.writeInterfaceToken(DESCRIPTOR);
            input.writeInt(type);
            input.writeString(name);
            input.writeString("");
            if (!binder.transact(1, input, output, 0)) throw new IllegalStateException("getter rejected");
            output.readException();
            String value = output.readString();
            if (output.dataAvail() != 0) throw new IllegalStateException("getter layout mismatch");
            return value;
        } finally { input.recycle(); output.recycle(); }
    }

    private static String value(IBinder binder) throws Exception {
        String value = get(binder, 0, KEY);
        if (!"0".equals(value) && !"1".equals(value)) throw new IllegalStateException("unknown/missing configuration");
        return value;
    }

    private static int integer(JSONObject object, String name) throws Exception {
        Object raw = object.get(name);
        if (!(raw instanceof Integer) && !(raw instanceof Long)) throw new IllegalStateException("integer metadata required");
        long number = ((Number)raw).longValue();
        if (number < Integer.MIN_VALUE || number > Integer.MAX_VALUE) throw new IllegalStateException("metadata overflow");
        return (int)number;
    }

    private static String nullable(JSONObject row, String name) throws Exception {
        if (!row.has(name) || row.isNull(name)) return null;
        Object object = row.get(name);
        if (!(object instanceof String)) throw new IllegalStateException("string metadata required");
        return (String)object;
    }

    private static JSONObject row(IBinder binder) throws Exception { return row(binder, true); }

    private static JSONObject row(IBinder binder, boolean requireScalar) throws Exception {
        String json = get(binder, 1, "{\"ConfigBean\":{\"configName\":\"" + KEY + "\"}}");
        if (json == null || json.length() > 8192) throw new IllegalStateException("metadata unavailable");
        JSONArray rows = new JSONArray(json);
        if (rows.length() != 1) throw new IllegalStateException("configuration must exist uniquely");
        JSONObject row = rows.getJSONObject(0);
        String rawValue = nullable(row, "configValue");
        if (!KEY.equals(row.getString("configName")) || integer(row, "canWriteOfPXR") == 0 ||
            (!"0".equals(rawValue) && !"1".equals(rawValue)) ||
            (requireScalar && !value(binder).equals(rawValue))) throw new IllegalStateException("configuration/metadata mismatch");
        if (row.has("children") && !row.isNull("children") && row.getJSONArray("children").length() != 0) {
            throw new IllegalStateException("group configuration unsupported");
        }
        integer(row, "level"); nullable(row, "category"); nullable(row, "token");
        return row;
    }

    private static boolean ignored(String key) {
        return key.equals("configValue") || key.equals("updateTime") || key.equals("updateClientId");
    }

    private static boolean equal(Object a, Object b) throws Exception {
        if (a == JSONObject.NULL || a == null) return b == JSONObject.NULL || b == null;
        if (a instanceof JSONObject && b instanceof JSONObject) {
            JSONObject first = (JSONObject)a, second = (JSONObject)b;
            Set<String> keys = new HashSet<String>();
            Iterator<String> iterator = first.keys(); while (iterator.hasNext()) keys.add(iterator.next());
            iterator = second.keys(); while (iterator.hasNext()) keys.add(iterator.next());
            for (String key : keys) {
                if (ignored(key)) continue;
                if (first.has(key) != second.has(key) || !equal(first.opt(key), second.opt(key))) return false;
            }
            return true;
        }
        if (a instanceof JSONArray && b instanceof JSONArray) {
            JSONArray first = (JSONArray)a, second = (JSONArray)b;
            if (first.length() != second.length()) return false;
            for (int i = 0; i < first.length(); ++i) if (!equal(first.get(i), second.get(i))) return false;
            return true;
        }
        return a.getClass().equals(b == null ? null : b.getClass()) && a.equals(b);
    }

    private static void privateRoot() throws Exception {
        StructStat stat = Os.lstat(ROOT);
        if (!OsConstants.S_ISDIR(stat.st_mode) || stat.st_uid != 0 || (stat.st_mode & 0077) != 0) {
            throw new IllegalStateException("unsafe recovery directory");
        }
    }

    private static void snapshot(JSONObject row) throws Exception {
        privateRoot();
        String temporary = ROOT + "/configuration.tmp-" + Process.myPid() + "-" + UUID.randomUUID();
        FileDescriptor fd = Os.open(temporary, OsConstants.O_WRONLY | OsConstants.O_CREAT | OsConstants.O_EXCL | OsConstants.O_NOFOLLOW, 0600);
        boolean published = false;
        try {
            try (FileOutputStream file = new FileOutputStream(fd)) {
                JSONObject record = new JSONObject(); record.put("backend", "configuration-service-v1"); record.put("row", row);
                file.write(record.toString().getBytes(StandardCharsets.UTF_8)); file.flush(); file.getFD().sync();
            }
            Os.rename(temporary, SNAPSHOT);
            published = true;
            FileDescriptor directory = Os.open(ROOT, OsConstants.O_RDONLY | OsConstants.O_NOFOLLOW, 0);
            try {
                if (!OsConstants.S_ISDIR(Os.fstat(directory).st_mode)) throw new IllegalStateException("directory replaced");
                Os.fsync(directory);
            } finally { Os.close(directory); }
        } finally {
            if (!published) {
                if (!new java.io.File(temporary).delete()) System.err.println("configuration snapshot temporary cleanup failed");
            }
        }
    }

    private static JSONObject baseline() throws Exception {
        privateRoot();
        FileDescriptor fd = Os.open(SNAPSHOT, OsConstants.O_RDONLY | OsConstants.O_NOFOLLOW, 0);
        try (FileInputStream file = new FileInputStream(fd)) {
            StructStat stat = Os.fstat(fd);
            if (!OsConstants.S_ISREG(stat.st_mode) || stat.st_uid != 0 || stat.st_nlink != 1 ||
                (stat.st_mode & 0077) != 0 || stat.st_size <= 0 || stat.st_size > 8192) throw new IllegalStateException("unsafe snapshot");
            byte[] data = new byte[(int)stat.st_size]; int used = 0;
            while (used < data.length) { int size = file.read(data, used, data.length - used); if (size <= 0) throw new IllegalStateException("short snapshot"); used += size; }
            JSONObject record = new JSONObject(new String(data, StandardCharsets.UTF_8));
            if (!"configuration-service-v1".equals(record.getString("backend"))) throw new IllegalStateException("wrong backend snapshot");
            return record.getJSONObject("row");
        }
    }

    private static void set(IBinder binder, String wanted, String expected) throws Exception { set(binder, wanted, expected, 4); }

    private static void set(IBinder binder, String wanted, String expected, int transaction) throws Exception {
        if (uncertainWrite) throw new IllegalStateException("prior remote write completion unknown");
        JSONObject original = baseline();
        JSONObject current = row(binder, false);
        if (!expected.equals(current.getString("configValue")) || !equal(original, current)) {
            throw new IllegalStateException("configuration ownership changed");
        }
        // Re-read immediately before the setter; the API has no atomic CAS.
        JSONObject fresh = row(binder, false);
        if (!expected.equals(fresh.getString("configValue")) || !equal(original, fresh)) throw new IllegalStateException("value/metadata changed before write");
        Parcel input = Parcel.obtain(); Parcel output = Parcel.obtain();
        try {
            input.writeInterfaceToken(DESCRIPTOR);
            input.writeString("com.pvr.configuration"); input.writeInt(0);
            input.writeString(KEY + "," + wanted);
            input.writeString(nullable(original, "category"));
            input.writeString(nullable(original, "token"));
            input.writeInt(integer(original, "level"));
            uncertainWrite = true;
            boolean accepted = binder.transact(transaction, input, output, 0);
            uncertainWrite = false; // Synchronous service handler has returned.
            if (!accepted) throw new IllegalStateException("setter rejected");
            output.readException();
            if (output.dataAvail() != 4 || output.readInt() != 1 || output.dataAvail() != 0) {
                throw new IllegalStateException("setter failed; notification may already have occurred");
            }
        } finally { input.recycle(); output.recycle(); }
        JSONObject after = row(binder);
        if (!wanted.equals(after.getString("configValue")) || !equal(original, after)) {
            throw new IllegalStateException("value/metadata readback mismatch");
        }
    }

    public static String execute(String[] args) throws Exception {
        if (Process.myUid() != 0 || args.length < 1 || args.length > 2) {
            throw new IllegalStateException("root and fixed operation required");
        }
        IBinder binder = service(); String command = args[0];
        if (args.length == 1 && command.equals("inspect")) return row(binder).toString();
        if (args.length == 1 && command.equals("raw")) return "configuration=" + row(binder, false).getString("configValue");
        if (args.length == 1 && command.equals("snapshot")) { JSONObject current = row(binder); snapshot(current); return "configuration=" + current.getString("configValue"); }
        if (args.length == 1 && command.equals("baseline")) return "configuration=" + baseline().getString("configValue");
        if (args.length == 1 && command.equals("verify")) {
            if (!equal(baseline(), row(binder))) throw new IllegalStateException("metadata ownership mismatch");
            return "configuration=" + value(binder);
        }
        if (args.length == 1 && command.equals("renotify-baseline")) {
            // Not a fallback for a failed mode write. Anchor the ordinary
            // cache at the already-restored, verified baseline first; a
            // same-value direct write then re-emits its notification without
            // leaving that cache inconsistent with the database.
            String original = baseline().getString("configValue");
            if (!original.equals(row(binder).getString("configValue"))) throw new IllegalStateException("baseline not restored");
            set(binder, original, original, 4);
            set(binder, original, original, 2);
            return "configuration=" + value(binder);
        }
        if (args.length == 2 && (command.equals("set-0") || command.equals("set-1")) && (args[1].equals("0") || args[1].equals("1"))) {
            set(binder, command.substring(4), args[1]);
        } else if (!(args.length == 1 && command.equals("query"))) throw new IllegalStateException("unsupported operation");
        return "configuration=" + value(binder);
    }

    public static void main(String[] args) {
        try {
            System.out.println(execute(args));
        } catch (Exception exception) {
            System.err.println("configuration control failed: " + exception.getClass().getSimpleName()); System.exit(2);
        }
    }
}
