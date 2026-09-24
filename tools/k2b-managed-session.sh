#!/bin/sh
# Recovery belongs to PID 1, not to this terminal or its shell traps.
set -eu
[ "$#" -ge 3 ] || { echo 'Usage: k2b-managed-session.sh UNIT POST_SCRIPT COMMAND [ARGS...]' >&2; exit 2; }
[ "$(id -u)" = 0 ] || { echo 'Run with sudo.' >&2; exit 1; }
unit=$1
post=$2
shift 2
case "$unit" in ''|*[!a-zA-Z0-9_.-]*) echo 'Invalid unit name' >&2; exit 2;; esac
case "$unit" in *.service) ;; *) echo 'Unit must end with .service' >&2; exit 2;; esac
# ExecStopPost is parsed by systemd, not a shell. Our project paths are simple.
case "$post" in /*) ;; *) echo 'Post script must be absolute' >&2; exit 2;; esac
case "$post" in *[!a-zA-Z0-9_./-]*) echo 'Unsupported post script path' >&2; exit 2;; esac
[ -r "$post" ] || { echo "Missing post script: $post" >&2; exit 1; }
exec 9>"/run/lock/$unit.lock"
flock -n 9 || { echo "Session already running: $unit" >&2; exit 1; }
[ "$(systemctl show "$unit" -p LoadState --value)" = not-found ] || {
    echo "Session still loaded; inspect with systemctl status $unit" >&2; exit 1;
}
cursor=$(mktemp "/run/lock/$unit.journal.XXXXXX")
cleanup() { rm -f -- "$cursor"; }
trap cleanup EXIT
stop_requested=0
stop_session() {
    stop_requested=1
    systemctl stop --no-block "$unit" 2>/dev/null || :
}
trap stop_session INT TERM HUP
journalctl -q -u "$unit" --cursor-file="$cursor" -n 0 --no-pager >/dev/null || :
echo "K2B managed session: $unit (Ctrl+C stops; logs: journalctl -u $unit)"
# systemd 249 expands $VARIABLE in ExecStart arguments even though no shell is
# used. Escape dollars so caller-supplied app names/options remain literal.
remaining=$#
while [ "$remaining" -gt 0 ]; do
    argument=$(printf '%sX' "$1" | sed 's/\$/$$/g')
    argument=${argument%X}
    shift
    set -- "$@" "$argument"
    remaining=$((remaining - 1))
done
systemd-run --quiet --unit="$unit" --collect --wait --service-type=exec \
    --property="ExecStopPost=/bin/sh $post" \
    --property=Restart=no --property=SendSIGKILL=no --property=TimeoutStopSec=15s \
    --property=StandardOutput=journal --property=StandardError=journal \
    --property=WorkingDirectory="$(pwd -P)" \
    --setenv="PULSE_SERVER=${PULSE_SERVER:-}" \
    --setenv="PULSE_COOKIE=${PULSE_COOKIE:-}" \
    -- "$@" 9>&- &
runner=$!
# Poll the journal cursor instead of leaving an orphaned journal follower when
# the launching shell is killed. The service does not inherit this lock FD.
while kill -0 "$runner" 2>/dev/null; do
    if [ "$stop_requested" = 1 ]; then
        systemctl stop --no-block "$unit" 2>/dev/null || :
    fi
    journalctl -q -u "$unit" --cursor-file="$cursor" --no-pager -o cat || :
    sleep 0.5 &
    wait "$!" || :
done
result=0
wait "$runner" || result=$?
journalctl -q -u "$unit" --cursor-file="$cursor" --no-pager -o cat || :
exit "$result"
