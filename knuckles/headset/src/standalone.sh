#!/system/bin/sh
# Fixed local management commands for the optional MixedVR root companion.
MODDIR=${0%/*}
ROOT=/data/adb/pico_mixedvr_standalone
BIN="$MODDIR/bin/pico-mixedvrd"

die() { echo "pico-mixedvr: $*" >&2; exit 1; }
[ "$(id -u)" = 0 ] || die "root required"
[ "$#" = 1 ] || die "usage: standalone.sh enable|start|stop|disable|restore|status"
[ -x "$BIN" ] || die "companion executable missing"

running() {
    [ -f "$ROOT/pid" ] || return 1
    PID=$(cat "$ROOT/pid")
    case "$PID" in ''|*[!0-9]*) return 1 ;; esac
    [ "$(readlink "/proc/$PID/exe" 2>/dev/null)" = "$BIN" ]
}

stop_helper() {
    if running; then
        kill -TERM "$PID" || return 1
        count=0
        while running; do
            count=$((count + 1))
            [ "$count" -lt 20 ] || { echo "helper did not stop; baseline retained" >&2; return 1; }
            sleep 1
        done
    fi
    rm -f "$ROOT/pid"
    "$BIN" --restore
}

start_helper() {
    [ -f "$ROOT/enabled" ] || die "not enabled"
    if ! running; then
        # The executable's kernel supervisor lock serializes concurrent starts;
        # only its owner writes pid/ready, never the shell's losing child.
        nohup "$BIN" --supervise </dev/null >>"$ROOT/companion.log" 2>&1 &
    fi
    count=0
    while [ "$count" -lt 15 ]; do
        if running && [ "$(cat "$ROOT/ready" 2>/dev/null)" = "$PID" ]; then
            return 0
        fi
        count=$((count + 1))
        sleep 1
    done
    die "companion not ready; inspect $ROOT/companion.log"
}

case "$1" in
    enable)
        [ ! -L "$ROOT" ] || die "state directory must not be a symlink"
        mkdir -p "$ROOT" || die "cannot create state directory"
        chmod 0700 "$ROOT" || die "cannot secure state directory"
        if [ ! -f "$ROOT/token" ]; then
            umask 077
            od -An -N32 -tx1 /dev/urandom | tr -d ' \n' >"$ROOT/token.tmp" || die "token generation failed"
            [ "$(wc -c <"$ROOT/token.tmp")" -eq 64 ] || die "invalid token length"
            printf '\n' >>"$ROOT/token.tmp"
            mv "$ROOT/token.tmp" "$ROOT/token" || die "token install failed"
        fi
        chmod 0600 "$ROOT/token" || die "cannot secure token"
        # Retain a fixed recovery executable if a module uninstall is interrupted.
        cp "$BIN" "$ROOT/recovery-helper.tmp" || die "cannot stage recovery executable"
        chmod 0700 "$ROOT/recovery-helper.tmp" || die "cannot secure recovery executable"
        mv "$ROOT/recovery-helper.tmp" "$ROOT/recovery-helper" || die "cannot retain recovery executable"
        cp "$MODDIR/bin/tracking-control.jar" "$ROOT/tracking-control.jar.tmp" || die "cannot retain recovery Binder helper"
        chmod 0600 "$ROOT/tracking-control.jar.tmp"
        mv "$ROOT/tracking-control.jar.tmp" "$ROOT/tracking-control.jar" || die "cannot retain recovery Binder helper"
        touch "$ROOT/enabled" || die "cannot enable companion"
        chmod 0600 "$ROOT/enabled"
        printf '1\n' >"$ROOT/requested" || die "cannot save enable choice"
        chmod 0600 "$ROOT/requested"
        echo "enabled; standalone LAN control starts only with a valid root-owned network file"
        ;;
    start) start_helper ;;
    stop) stop_helper || die "recovery failed; baseline journal retained" ;;
    disable)
        mkdir -p "$ROOT" || die "cannot save disable choice"
        chmod 0700 "$ROOT"
        umask 077
        printf '0\n' >"$ROOT/requested" || die "cannot save disable choice"
        rm -f "$ROOT/enabled"
        stop_helper || die "recovery failed; baseline journal retained"
        ;;
    restore)
        running && die "stop the companion before independent restore"
        "$BIN" --restore || die "recovery failed; baseline journal retained"
        ;;
    status)
        if running; then
            echo "running pid=$PID (use PC status for layer observations)"
            [ -f "$ROOT/baseline" ] && echo "recovery journal present"
            :
        else
            "$BIN" --status || die "status read failed"
        fi
        ;;
    *) die "unknown operation" ;;
esac
