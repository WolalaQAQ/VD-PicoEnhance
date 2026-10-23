#!/system/bin/sh
# One boot dispatch; the existing native supervisor owns the service lifecycle.
MODDIR=${0%/*}
ROOT=/data/adb/pico_mixedvr_standalone

while [ "$(getprop sys.boot_completed)" != 1 ]; do
    sleep 1
done
boot_enabled() {
    [ ! -f "$MODDIR/disable" ] && [ ! -f "$MODDIR/remove" ] || return 1
    if [ -e "$ROOT/requested" ]; then
        requested=$(cat "$ROOT/requested") || exit 1
        case "$requested" in
            0) return 1 ;;
            1) return 0 ;;
            *) echo "pico-mixedvr: invalid saved requested choice; not starting" >&2; exit 1 ;;
        esac
    fi
    # Installer opt-in is explicit. A pre-existing enabled marker is retained.
    [ -f "$ROOT/enabled" ]
}

boot_enabled || exit 0
if [ -d /data/adb/vdhs_mixedvr ] && [ -f /data/adb/modules/vdhs_zygisk/mixedvr.sh ]; then
    sh /data/adb/modules/vdhs_zygisk/mixedvr.sh disable || exit 1
fi
# Retiring the old owner may take time; do not overwrite a newer stop choice.
boot_enabled || exit 0

# Refresh root recovery copies through the unchanged manager, then start once.
sh "$MODDIR/standalone.sh" enable && sh "$MODDIR/standalone.sh" start
