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
ui_print "- VD-only hand-mode startup support is enabled on the supported PICO firmware; APKs and global tracking stay unchanged."
ui_print "- System gesture suppression is controlled only by VD's Input-tab switch (default: off)."
ui_print "- Hand mesh is bundled and installed on first launch if missing."
ui_print "- SteamVR hand passthrough follows VD's own VR hand-passthrough setting."
ui_print "- PICO battery forwarding is enabled; install the PC battery patch with the unified installer."
ui_print "- PicoHand<->Knuckles is a separate optional module, not part of the base input path."

# Stop the old executable before Magisk replaces its module files. Its durable
# pairing/enable state is retained; the new service starts it after reboot.
OLD_MODULE=/data/adb/modules/vdhs_zygisk
if [ -f "$OLD_MODULE/mixedvr.sh" ] && [ -d /data/adb/vdhs_mixedvr ]; then
    sh "$OLD_MODULE/mixedvr.sh" disable || abort "! Old tracking owner restore failed; recovery files retained."
fi

# Keep an explicitly saved VD-only capability choice on upgrade.
OLD=/data/adb/modules/vdhs_zygisk/vd-capability.txt
if [ -f "$OLD" ] && [ ! -L "$OLD" ] && [ "$MODPATH/vd-capability.txt" != "$OLD" ]; then
    cp "$OLD" "$MODPATH/vd-capability.txt" || abort "! Cannot preserve VD capability configuration."
fi

# Payload files must be readable when the module dir is opened before
# specialization; keep the standard Magisk ownership/mode.
set_perm_recursive "$MODPATH" 0 0 0755 0644
set_perm_recursive "$MODPATH/payload" 0 0 0755 0644
set_perm "$MODPATH/zygisk/arm64-v8a.so" 0 0 0644
set_perm "$MODPATH/coldstart.sh" 0 0 0755
set_perm "$MODPATH/uninstall.sh" 0 0 0755
set_perm "$MODPATH/mixedvr.sh" 0 0 0755
set_perm "$MODPATH/bin/vdhs-mixedvrd" 0 0 0755
