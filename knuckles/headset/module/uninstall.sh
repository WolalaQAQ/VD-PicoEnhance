#!/system/bin/sh
MODDIR=${0%/*}
ROOT=/data/adb/pico_mixedvr_standalone

[ -d "$ROOT" ] || exit 0
# disable saves requested=0, stops the existing supervisor and runs its restore.
# Do not issue an independent restore after a failed stop or uncertain write.
if ! sh "$MODDIR/standalone.sh" disable; then
    echo "pico-mixedvr: uninstall recovery incomplete; root recovery-helper, jar and journals retained at $ROOT" >&2
    exit 1
fi
# Keep ROOT, pairing, the last metadata snapshot and recovery copies.
exit 0
