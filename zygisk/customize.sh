#!/system/bin/sh
# Magisk install-time script. Runs in the installer's context.
# No module.prop "zygisk" flag exists: Magisk auto-discovers zygisk/<abi>.so.
SKIPUNZIP=0

if [ "$ARCH" != "arm64" ]; then
  abort "! VD-PicoEnhance only supports arm64 (this is a PICO 4 Pro / phoenix build)."
fi

ui_print "- VD-PicoEnhance $MODVER"
ui_print "- target process: VirtualDesktop.Android (untouched official APK)"

# Payload files must be readable when the module dir is opened before
# specialization; keep the standard Magisk ownership/mode.
set_perm_recursive "$MODPATH" 0 0 0755 0644
set_perm_recursive "$MODPATH/payload" 0 0 0755 0644
set_perm "$MODPATH/zygisk/arm64-v8a.so" 0 0 0644
