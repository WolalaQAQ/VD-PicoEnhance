#!/system/bin/sh
SKIPUNZIP=0
[ "$ARCH" = arm64 ] || abort "! PicoHand<->Knuckles requires arm64."
OLD=/data/adb/modules/pico_mixedvr_standalone
if [ -x "$OLD/bin/pico-mixedvrd" ]; then
    sh "$OLD/standalone.sh" stop || abort "! Restore failed; recovery journal retained."
fi
set_perm_recursive "$MODPATH" 0 0 0755 0644
set_perm "$MODPATH/bin/pico-mixedvrd" 0 0 0755
for script in service.sh standalone.sh uninstall.sh; do
    set_perm "$MODPATH/$script" 0 0 0755
done
ui_print "- PicoHand<->Knuckles installed. The PC installer configures its LAN pairing."
ui_print "- Existing explicit disable choices are retained."
