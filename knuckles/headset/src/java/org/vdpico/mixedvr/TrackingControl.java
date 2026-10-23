package org.vdpico.mixedvr;

import android.os.IBinder;
import android.os.Parcel;
import android.os.Process;
import java.lang.reflect.Method;

/** Fixed Binder operations, launched by the root companion via app_process. */
public final class TrackingControl {
    private static final String SERVICE = "pvrtracking";
    private static final String DESCRIPTOR = "pvr.ITrackingService";

    public static String execute(String[] args) throws Exception {
        if (Process.myUid() != 0 || args.length != 1) {
            throw new IllegalStateException("root and fixed operation required");
        }
        String operation = args[0];
        if (!operation.equals("query") && !operation.equals("start") && !operation.equals("stop")) {
            throw new IllegalStateException("unsupported operation");
        }
        Parcel input = Parcel.obtain();
        Parcel output = Parcel.obtain();
        try {
            Class<?> manager = Class.forName("android.os.ServiceManager");
            Method check = manager.getDeclaredMethod("checkService", String.class);
            IBinder binder = (IBinder)check.invoke(null, SERVICE);
            if (binder == null || !DESCRIPTOR.equals(binder.getInterfaceDescriptor())) {
                throw new IllegalStateException("tracking service unavailable or descriptor mismatch");
            }
            input.writeInterfaceToken(DESCRIPTOR);
            if (operation.equals("query")) {
                input.writeInt(5);
                input.writeString("hand-tracking-enable");
                if (!binder.transact(8, input, output, 0)) throw new IllegalStateException("query rejected");
                output.readException();
                int value = output.readInt();
                if (output.dataAvail() != 0 || (value != 0 && value != 1)) {
                    throw new IllegalStateException("unexpected tracking configuration reply");
                }
                return "tracking_config=" + value;
            } else {
                int code = operation.equals("start") ? 20 : 21;
                if (!binder.transact(code, input, null, IBinder.FLAG_ONEWAY)) {
                    throw new IllegalStateException("tracking transaction rejected");
                }
                // Oneway acknowledgement is delivery, not algorithm-state proof.
                return "transaction=delivered";
            }
        } finally {
            input.recycle();
            output.recycle();
        }
    }

    public static void main(String[] args) {
        try { System.out.println(execute(args)); }
        catch (Exception exception) {
            System.err.println("tracking control failed: " + exception.getClass().getSimpleName()); System.exit(2);
        }
    }
}
