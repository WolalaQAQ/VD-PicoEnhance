#!/system/bin/sh
# late_start service: one log line so the module's load state is visible without
# pulling logcat, plus the gesture-switch mirror.
MODDIR=${0%/*}
echo "vdhs_zygisk: service.sh loaded $(date) module=$MODDIR" >> /data/adb/vdhs_zygisk.log

# Match vdhs_config.h: skip blank/comment lines, then accept one digit with an
# optional trailing comment. An invalid first value uses the caller's default.
read_setting() {
    blank=$(printf ' \t\r')
    while IFS= read -r line || [ -n "$line" ]; do
        line=${line%%#*}
        line=${line#"${line%%[!$blank]*}"}
        line=${line%"${line##*[!$blank]}"}
        [ -n "$line" ] || continue
        case "$line" in
            0|1|2) printf '%s\n' "$line" ;;
            *) printf '%s\n' "$2" ;;
        esac
        return
    done 2>/dev/null < "$1"
    printf '%s\n' "$2"
}

# Full mode supplies the Shell receiver. VD's saved checkbox is its only switch.
if [ "$(read_setting "$MODDIR/payload/mode.txt" 2)" != 2 ]; then
    if [ -f /data/adb/vdhs_mixedvr/pid ] || [ -f /data/adb/vdhs_mixedvr/baseline ]; then
        sh "$MODDIR/mixedvr.sh" stop >> /data/adb/vdhs_zygisk.log 2>&1
    fi
    exit 0
fi

# Retain the old recovery executable for upgrades, never start the old owner.
# The optional PicoHand<->Knuckles module has its own lifecycle and state root.
if [ -d /data/adb/vdhs_mixedvr ]; then
    sh "$MODDIR/mixedvr.sh" disable >> /data/adb/vdhs_zygisk.log 2>&1
fi

# The standalone module's migration guard needs the old choice saved first, so
# only the property mirror below waits for boot completion.
while [ "$(getprop sys.boot_completed)" != 1 ]; do
    sleep 1
done
echo "vdhs_zygisk: boot_completed=1; gesture mirror starting" >> /data/adb/vdhs_zygisk.log

# Virtual Desktop's Input tab now carries a "Suppress system gestures" switch.
# The app writes it to its own payload directory; the Shell payload reads the
# property below, so mirror one into the other. Missing or invalid state is OFF.
(
    last=uninitialized # Always overwrite/clear a property left by an earlier run.
    while true; do
        flag=$(tr -d '\r\n' 2>/dev/null < /data/data/VirtualDesktop.Android/vdhs/system_gestures.txt)
        case "$flag" in
            0|1) ;;
            *) flag=0 ;;
        esac
        if [ "$flag" != "$last" ] && setprop vdhs.system_gestures "$flag"; then
            last=$flag
        fi
        sleep 1
    done
) &
