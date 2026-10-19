package org.vdpico.capability;

import android.content.pm.ApplicationInfo;
import android.os.Bundle;

import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;

/** Fixed, bounded wire format shared by the system-server helper and root client. */
final class CapabilityProtocol {
    static final String SOCKET = "vdhs.vd-capability";
    static final String TARGET = "VirtualDesktop.Android";
    static final String KEY = "handtracking";
    static final String DESCRIPTOR = "android.content.pm.IPackageManager";
    static final String FINGERPRINT =
            "Pico/Phoenix/PICOA8110:10/5.13.7/smartcm.1761755159:user/dev-keys";
    static final long MAX_GENERATION = 0xffffffffL;

    private CapabilityProtocol() { }

    static boolean isCommand(String command) {
        return "status".equals(command) || "enable".equals(command)
                || "disable".equals(command);
    }

    @SuppressWarnings("deprecation") // API 29 needs an untyped read to reject Boolean/String/Long 1.
    static boolean isIntOne(Bundle metadata) {
        if (metadata == null || !metadata.containsKey(KEY)) {
            return false;
        }
        Object value = metadata.get(KEY);
        return value instanceof Integer && ((Integer) value).intValue() == 1;
    }

    static String actual(ApplicationInfo info) {
        if (info == null || info.metaData == null || !info.metaData.containsKey(KEY)) {
            return "missing";
        }
        return isIntOne(info.metaData) ? "int1" : "other";
    }

    /** A single printable ASCII line, LF terminated. CR, EOF and overlong lines fail. */
    static String readLine(InputStream input, int maxLength) throws IOException {
        byte[] bytes = new byte[maxLength];
        int count = 0;
        for (;;) {
            int next = input.read();
            if (next == '\n') {
                return new String(bytes, 0, count, StandardCharsets.US_ASCII);
            }
            if (next < 32 || next > 126 || count == maxLength) {
                throw new IOException("Invalid protocol line");
            }
            bytes[count++] = (byte) next;
        }
    }

    static boolean oneOf(String value, String... allowed) {
        for (String candidate : allowed) {
            if (candidate.equals(value)) {
                return true;
            }
        }
        return false;
    }

    static long unsigned(String value, long maximum) throws IOException {
        if (!value.matches("0|[1-9][0-9]{0,9}")) {
            throw new IOException("Invalid unsigned field");
        }
        long result;
        try {
            result = Long.parseLong(value);
        } catch (NumberFormatException error) {
            throw new IOException("Invalid unsigned field");
        }
        if (result > maximum) {
            throw new IOException("Unsigned field out of range");
        }
        return result;
    }
}
