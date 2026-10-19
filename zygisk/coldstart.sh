#!/system/bin/sh
# VD-only PackageManager metadata control. Never changes a global property.
MODDIR=${0%/*}
ROOT=/data/adb/vdhs-vd-capability
MODULE=/data/adb/modules/vdhs_zygisk
UPDATED=/data/adb/modules_update/vdhs_zygisk
ACTION=${1:-status}
umask 077

fail() { echo "vdhs-capability: $*" >&2; exit 1; }
[ "$(id -u)" = 0 ] || fail "root required"
[ "$#" -le 1 ] || fail "unexpected arguments"
case "$ACTION" in status|query|enable|disable|boot|recovercopy|legacy-restore) ;; *) fail "usage: coldstart.sh status|query|enable|disable|boot|recovercopy|legacy-restore" ;; esac

atomic_setting() {
    destination=$1
    value=$2
    [ ! -L "$destination" ] && [ ! -L "$destination.tmp" ] || fail "unsafe configuration path"
    printf 'VDC2 %s\n' "$value" >"$destination.tmp" &&
        chmod 0600 "$destination.tmp" && mv "$destination.tmp" "$destination" || fail "configuration publication failed"
}

read_setting() {
    [ -f "$1" ] && [ ! -L "$1" ] || return 1
    case "$(cat "$1")" in 'VDC2 0') printf 0 ;; 'VDC2 1') printf 1 ;; *) return 2 ;; esac
}

lock_state() {
    [ ! -L "$ROOT" ] || fail "unsafe state directory"
    mkdir -p "$ROOT" && chmod 0700 "$ROOT" || fail "cannot secure state directory"
    [ "$(stat -c %u "$ROOT")" = 0 ] || fail "state directory must be root-owned"
    [ ! -L "$ROOT/lock" ] || fail "unsafe lock"
    exec 9>>"$ROOT/lock" || fail "cannot open lock"
    flock -n 3 3>&9 || fail "another capability operation owns the lock"
}

client() {
    jar="$MODDIR/capability/control.jar"
    [ -f "$jar" ] || jar="$ROOT/control.jar"
    [ -f "$jar" ] && [ ! -L "$jar" ] || fail "capability client is missing"
    CLASSPATH="$jar" /system/bin/app_process /system/bin org.vdpico.capability.CapabilityClient "$1"
}

copy_recovery() {
    [ -f "$0" ] && [ ! -L "$0" ] || fail "invalid recovery source"
    for name in recovery.sh control.jar; do
        [ ! -L "$ROOT/$name" ] && [ ! -L "$ROOT/$name.tmp" ] || fail "unsafe recovery destination"
    done
    source_jar="$MODDIR/capability/control.jar"
    [ -f "$source_jar" ] || source_jar="$ROOT/control.jar"
    [ -f "$source_jar" ] && [ ! -L "$source_jar" ] || fail "recovery client missing"
    cp "$0" "$ROOT/recovery.sh.tmp" && chmod 0700 "$ROOT/recovery.sh.tmp" &&
        mv "$ROOT/recovery.sh.tmp" "$ROOT/recovery.sh" || fail "cannot retain recovery script"
    cp "$source_jar" "$ROOT/control.jar.tmp" && chmod 0600 "$ROOT/control.jar.tmp" &&
        mv "$ROOT/control.jar.tmp" "$ROOT/control.jar" || fail "cannot retain recovery client"
    sync || fail "cannot commit recovery files"
}

publish_request() {
    atomic_setting "$ROOT/requested" "$1"
    for directory in "$MODDIR" "$MODULE" "$UPDATED"; do
        if [ -f "$directory/module.prop" ]; then atomic_setting "$directory/vd-capability.txt" "$1"; fi
    done
    sync || fail "configuration durability failed"
}

case "$ACTION" in
    status)
        requested=$(read_setting "$ROOT/requested")
        case "$?" in
            0) ;;
            1)
                requested=$(read_setting "$MODDIR/vd-capability.txt")
                case "$?" in 0) ;; 1) requested=1 ;; *) fail "invalid module request" ;; esac
                ;;
            *) fail "invalid saved request" ;;
        esac
        echo "vdhs-capability: saved_request=$requested; global launch property is not changed"
        client status
        exit $?
        ;;
    query) client metadata-query; exit $? ;;
    legacy-restore)
        legacy=/data/adb/vdhs-coldstart/recovery.sh
        [ -f "$legacy" ] && [ ! -L "$legacy" ] || fail "no legacy property baseline/recovery script"
        sh "$legacy" disable
        exit $?
        ;;
    boot)
        [ -e "$ROOT/requested" ] || exit 0
        lock_state
        setting=$(read_setting "$ROOT/requested") || fail "invalid boot request"
        atomic_setting "$MODDIR/vd-capability.txt" "$setting"
        sync || fail "boot request publication failed"
        exit 0
        ;;
esac

lock_state
case "$ACTION" in
    recovercopy) copy_recovery ;;
    enable)
        [ -f "$MODDIR/payload/mode.txt" ] || fail "enable must run from the installed full module"
        mode=$(sed -n '/^[[:space:]]*[12][[:space:]]*$/p' "$MODDIR/payload/mode.txt" | tr -d '[:space:]')
        [ "$mode" = 2 ] || fail "VD declaration requires full payload mode 2"
        copy_recovery
        publish_request 1
        client enable
        result=$?
        case "$result" in
            0) echo "vdhs-capability: VD-only request accepted; applied/native readback is required before launch" ;;
            3) echo "vdhs-capability: request saved; reboot to load the module's system_server helper" ;;
            *) fail "live capability operation failed; request remains saved, inspect status or disable" ;;
        esac
        ;;
    disable)
        publish_request 0
        # Disarming must succeed even if preserving the client/script fails.
        # No boot process can republish a stale enabled request after this lock.
        copy_recovery
        client disable
        result=$?
        case "$result" in
            0) echo "vdhs-capability: live VD declaration restore acknowledged" ;;
            3) echo "vdhs-capability: future injection disabled; reboot restores APK-original metadata if live helper is unavailable" ;;
            *) fail "live declaration recovery failed; request disarmed, reboot restores original metadata" ;;
        esac
        ;;
esac
