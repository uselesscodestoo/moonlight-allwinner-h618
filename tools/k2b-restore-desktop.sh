#!/bin/sh
# Restore the existing Longan fbdev/XFCE session, without opening /dev/disp.
set -eu
fail() { echo "K2B desktop restore failed: $*" >&2; exit 1; }
[ "$(id -u)" = 0 ] || fail 'run with sudo'
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
helper="$root/build/k2b-integrated/k2b-fb-unblank"
debug=/sys/kernel/debug/dispdbg
[ -x "$helper" ] || fail "missing $helper; rebuild the K2B target"
command -v fuser >/dev/null || fail 'fuser is required to check display ownership'
[ -c /dev/disp ] && [ -c /dev/fb0 ] || fail 'display devices unavailable'
if pgrep -x moonlight >/dev/null || fuser /dev/disp >/dev/null 2>&1; then
    fail 'display busy; refusing to take over a running stream/display user'
fi
for node in name command param start; do
    [ -w "$debug/$node" ] || fail "unavailable control: $debug/$node"
done
echo "K2B desktop restore: service=${SERVICE_RESULT:-manual} exit=${EXIT_CODE:-none}/${EXIT_STATUS:-none}"
# Unlike switch/switch1, blank 0 retains the current RGB/timing configuration.
printf 'disp0\n' > "$debug/name"
printf 'blank\n' > "$debug/command"
printf '0\n' > "$debug/param"
printf '1\n' > "$debug/start"
"$helper" || fail 'fb0 unblank ioctl failed'
# HDMI lock can take a moment after enable. Readback is not optical acceptance.
attempt=0
while [ "$attempt" -lt 30 ]; do
    hdmi=$(cat /sys/class/hdmi/hdmi/attr/hdmi_source)
    layers=$(cat /sys/class/disp/disp/attr/sys)
    if printf '%s\n' "$hdmi" | grep -q 'PhyPower:  *1' &&
       printf '%s\n' "$hdmi" | grep -q 'PhyLock:  *1' &&
       printf '%s\n' "$layers" | grep -q 'enable ch\[1\] lyr\[0\]'; then
        echo 'K2B desktop restored: HDMI locked, fb0 layer enabled'
        exit 0
    fi
    attempt=$((attempt + 1))
    sleep 0.1
done
fail 'HDMI lock or fb0 layer readback did not become ready'
