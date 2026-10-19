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
[ "$(read_setting "$MODDIR/payload/mode.txt" 2)" = 2 ] || exit 0

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
