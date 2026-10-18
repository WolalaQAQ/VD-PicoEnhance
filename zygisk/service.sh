#!/system/bin/sh
# late_start service: one log line so the module's load state is visible without
# pulling logcat, plus the gesture-switch mirror.
MODDIR=${0%/*}
echo "vdhs_zygisk: service.sh loaded $(date) module=$MODDIR" >> /data/adb/vdhs_zygisk.log

# Virtual Desktop's Input tab now carries a "Suppress system gestures" switch.
# The app writes it to its own payload directory; the Shell payload reads the
# property below, so mirror one into the other. Without a value the Shell keeps
# using payload/system_gestures.txt as before.
(
    while true; do
        flag=$(tr -d '\r\n' < /data/data/VirtualDesktop.Android/vdhs/system_gestures.txt 2>/dev/null)
        case "$flag" in
            0|1) setprop vdhs.system_gestures "$flag" ;;
            *) setprop vdhs.system_gestures "" ;;
        esac
        sleep 1
    done
) &
