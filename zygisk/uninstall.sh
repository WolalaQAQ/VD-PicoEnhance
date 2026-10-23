#!/system/bin/sh
# Restore only state owned by the two optional helpers; retain recovery journals
# on failure rather than deleting the evidence needed for a later restore.
MODDIR=${0%/*}
result=0
if [ -f /data/adb/vdhs_mixedvr/enabled ] || [ -f /data/adb/vdhs_mixedvr/pid ] || [ -f /data/adb/vdhs_mixedvr/baseline ]; then
    sh "$MODDIR/mixedvr.sh" disable || result=1
fi
sh "$MODDIR/coldstart.sh" disable || {
    echo "VD-PicoEnhance: capability restore incomplete; reboot restores APK-original metadata" >&2
    result=1
}
exit "$result"
