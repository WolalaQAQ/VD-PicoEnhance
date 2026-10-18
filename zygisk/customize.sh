#!/system/bin/sh
# Magisk install-time script. Runs in the installer's context.
# No module.prop "zygisk" flag exists: Magisk auto-discovers zygisk/<abi>.so.
SKIPUNZIP=0

if [ "$ARCH" != "arm64" ]; then
  abort "! VD-PicoEnhance only supports arm64."
fi

ui_print "- VD-PicoEnhance $MODVER"
ui_print "- target process: VirtualDesktop.Android (untouched official APK)"
ui_print "- VD enhancements are enabled by default; existing hand_gesture.txt overrides are kept."
ui_print "- Shell gesture suppression is off by default (payload/system_gestures.txt); reboot after changes."
ui_print "- Hand mesh is bundled and installed on first launch if missing."
ui_print "- SteamVR hand passthrough follows VD's own VR hand-passthrough setting."

OLD_GESTURES=/data/adb/modules/vdhs_zygisk/payload/system_gestures.txt
if [ -f "$OLD_GESTURES" ] && [ ! -L "$OLD_GESTURES" ] && [ "$MODPATH/payload/system_gestures.txt" != "$OLD_GESTURES" ]; then
    cp "$OLD_GESTURES" "$MODPATH/payload/system_gestures.txt" || abort "! Cannot preserve Shell gesture configuration."
fi

# Payload files must be readable when the module dir is opened before
# specialization; keep the standard Magisk ownership/mode.
set_perm_recursive "$MODPATH" 0 0 0755 0644
set_perm_recursive "$MODPATH/payload" 0 0 0755 0644
set_perm "$MODPATH/zygisk/arm64-v8a.so" 0 0 0644
