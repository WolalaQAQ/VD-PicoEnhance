package org.vdpico.mixedvr;

import android.os.Process;
import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;

/** Private parent/child pipe; retains one VM rather than starting one per tick. */
public final class ControlSession {
    public static void main(String[] args) throws Exception {
        if (Process.myUid() != 0 || args.length != 0) throw new IllegalStateException("root-only pipe session");
        BufferedReader input = new BufferedReader(new InputStreamReader(System.in, StandardCharsets.UTF_8));
        String line;
        while ((line = input.readLine()) != null) {
            String id = "0";
            try {
                if (line.length() > 120 || line.indexOf('\r') >= 0) throw new IllegalStateException("bad framing");
                String[] fields = line.split(" ", -1);
                if (fields.length < 2 || fields.length > 4 || !fields[0].matches("[0-9]{1,20}")) throw new IllegalStateException("bad operation");
                id = fields[0];
                if (fields.length == 2 && fields[1].equals("B")) {
                    if (ConfigurationControl.hasUncertainWrite()) throw new IllegalStateException("unresolved remote write");
                    System.out.println(id + " OK barrier"); System.out.flush(); continue;
                }
                String[] command = Arrays.copyOfRange(fields, 2, fields.length);
                String result;
                if (fields[1].equals("C")) result = ConfigurationControl.execute(command);
                else if (fields[1].equals("T")) result = TrackingControl.execute(command);
                else throw new IllegalStateException("unknown subsystem");
                if (result.indexOf('\n') >= 0 || result.indexOf('\r') >= 0) throw new IllegalStateException("bad reply");
                System.out.println(id + " OK " + result);
            } catch (Exception exception) {
                System.out.println(id + " ERR " + exception.getClass().getSimpleName());
            }
            System.out.flush();
        }
    }
}
