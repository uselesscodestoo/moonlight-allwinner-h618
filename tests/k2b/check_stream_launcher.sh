#!/bin/sh
# Native-board integration check. Inspect the actual launch environment/argv;
# intercept only exec so no VPU/display device is opened by this test.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd -P)
test "$(id -u)" = 0 && test -n "${SUDO_UID:-}" || {
    echo "Run with sudo from the board desktop user." >&2
    exit 1
}
test -S "/run/user/$SUDO_UID/pulse/native"
for expected in pulse hw:1,0 null; do
    set -- 192.168.137.1
    if [ "$expected" != pulse ]; then set -- "$@" -audio "$expected"; fi
    env -u PULSE_SERVER -u PULSE_COOKIE K2B_EXPECT_AUDIO="$expected" bash -c '
        exec() {
            expected_server="unix:/run/user/$SUDO_UID/pulse/native"
            test "${PULSE_SERVER:-}" = "$expected_server" || {
                echo "FAIL: launcher did not select the sudo user Pulse server" >&2
                exit 1
            }
            expected_home=$(getent passwd "$SUDO_UID" | cut -d: -f6)
            test "${PULSE_COOKIE:-}" = "$expected_home/.config/pulse/cookie"
            audio=
            while [ "$#" -gt 0 ]; do
                if [ "$1" = -audio ]; then shift; audio=$1; fi
                shift
            done
            test "$audio" = "$K2B_EXPECT_AUDIO" || {
                echo "FAIL: expected audio $K2B_EXPECT_AUDIO, got $audio" >&2
                exit 1
            }
            echo "PASS: launcher audio=$audio with sudo user Pulse environment"
            exit 0
        }
        . "$0"
    ' "$root/tools/k2b-stream.sh" "$@"
done
