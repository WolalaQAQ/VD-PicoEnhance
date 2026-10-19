#!/system/bin/sh
# Disarm only the VD metadata declaration; no MixedVR or global tracking control.
MODDIR=${0%/*}
sh "$MODDIR/coldstart.sh" disable || {
    echo "VD-PicoEnhance: capability restore incomplete; reboot restores APK-original metadata" >&2
    exit 1
}
