#!/system/bin/sh
# post-fs-data: nothing to mount.
#
# This module is pure Zygisk injection: Magisk loads zygisk/arm64-v8a.so into
# the target process; the module stages payload/ into the app's data dir at
# runtime. There is intentionally no boot-time action here.
MODDIR=${0%/*}
if [ ! -f "$MODDIR/zygisk/arm64-v8a.so" ]; then
  echo "vdhs_zygisk: zygisk/arm64-v8a.so is missing" > /dev/kmsg 2>/dev/null
fi
