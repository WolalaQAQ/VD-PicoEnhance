#!/system/bin/sh
# post-fs-data: nothing to mount.
#
# This module is pure Zygisk injection: Magisk loads zygisk/arm64-v8a.so into
# the target process; the module stages payload/ into the app's data dir at
# runtime. Restore the saved VD-only capability request before server loading.
MODDIR=${0%/*}
sh "$MODDIR/coldstart.sh" boot >> /data/adb/vdhs_zygisk.log 2>&1
if [ ! -f "$MODDIR/zygisk/arm64-v8a.so" ]; then
  echo "vdhs_zygisk: zygisk/arm64-v8a.so is missing" > /dev/kmsg 2>/dev/null
fi
