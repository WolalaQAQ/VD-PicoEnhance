#!/system/bin/sh
# late_start service: no daemon. One log line so the module's load state is
# visible without pulling logcat.
MODDIR=${0%/*}
echo "vdhs_zygisk: service.sh loaded $(date) module=$MODDIR" >> /data/adb/vdhs_zygisk.log
