#!/bin/bash
# Reference quick-start launcher for the Orange Pi Zero 2W (H618, cedrus VPU).
#
# This is the script used on the reference board, copied into the repository as
# an example.  It is deliberately mostly self-contained: it expects this
# repository to be built in place (build/moonlight) and the companion
# libva-v4l2-request driver to be reachable either through LIBVA_DRIVERS_PATH or
# next to this repository (see DRIVER_DIR below).  Adapt the host/app defaults
# and drop the board-specific parts (panfrost reload, VT/lock diagnostics,
# xfwm4 compositor toggle) if they do not apply to your system.
#
# Board-specific notes (see docs/h618-zero-copy.md in this repository for the
# measurements and the full list of known issues):
#
#  * Hardware decode uses the custom libva-v4l2-request driver in
#    ~/Downloads/v4l2-dri.  It asks the kernel for plain linear NV12 capture
#    whenever the width is a multiple of 32 (1280/1920), which lets moonlight
#    import the decoder's dma-buf straight into the GPU (zero-copy) instead of
#    reading it back over the CPU (that readback alone costs ~55 ms/frame at
#    1080p, uncached).  No extra environment variable is needed; the tiled
#    capture format is only used for other widths (with a slower CPU fallback).
#
#  * xfwm4's compositor costs ~14 ms per 1080p frame on this SoC, which caps a
#    fullscreen 1080p client at ~29 fps.  With compositing off the same client
#    reaches ~48-53 fps, so it is turned off for the duration of the stream and
#    restored afterwards.
#
#  * The screen saver and DPMS are switched off for good (see below and
#    ~/.config/autostart/never-blank.desktop): an idle blank locks the session
#    through light-locker and makes lightdm switch to its greeter VT, which
#    suspends AIGLX on this VT and pushes every client onto llvmpipe.  A blank
#    during a stream costs ~750 ms per swap on top of that.
#
#  * panfrost occasionally fails to probe at boot, leaving no
#    /dev/dri/renderD* and silently pushing everything onto llvmpipe.
#
# The stream output is tee'd to ~/.moonlight-last.log (LOG=... to change) and a
# short summary - average/peak fps, per-frame draw cost and the host:client
# frame ratio - is printed when it ends, so "did the client keep up?" is one
# command away.
#
# Usage:  ./stream-desktop.sh [720|1080] [fps]
#     or:  RES=-720 FPS=30 CODEC=h264 APP=Desktop HOST=1.2.3.4 ./stream-desktop.sh
# Stop with Ctrl+C (or the in-stream hotkey Ctrl+Alt+Shift+Q).

set -u

HOST=${HOST:-172.31.5.252}
APP=${APP:-Desktop}
CODEC=${CODEC:-h265}
FPS=${FPS:-60}
LOCALAUDIO=${LOCALAUDIO:-1}
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
MOONLIGHT="${MOONLIGHT:-$SCRIPT_DIR/../build/moonlight}"
# Where the v4l2_request driver .so lives: LIBVA_DRIVERS_PATH wins, otherwise
# look next to this repository (the layout used on the reference board:
# ~/Downloads/{libva-v4l2-request,moonlight-embedded,v4l2-dri}).
if [ -z "${LIBVA_DRIVERS_PATH:-}" ]; then
    for candidate in "$SCRIPT_DIR/../../v4l2-dri" \
                     "$SCRIPT_DIR/../../libva-v4l2-request/build/src"; do
        [ -f "$candidate/v4l2_request_drv_video.so" ] && LIBVA_DRIVERS_PATH="$candidate" && break
    done
fi
if [ -z "${LIBVA_DRIVERS_PATH:-}" ] || [ ! -f "$LIBVA_DRIVERS_PATH/v4l2_request_drv_video.so" ]; then
    echo "stream-desktop: v4l2_request_drv_video.so not found; set LIBVA_DRIVERS_PATH" >&2
    exit 1
fi
export LIBVA_DRIVERS_PATH

# Optional positional overrides: 720/1080 and fps.
case "${1:-}" in
    720)  RES="-720" ;;
    1080) RES="-1080" ;;
    "")   RES="${RES:--1080}" ;;
    *)    echo "usage: $0 [720|1080] [fps]" >&2; exit 2 ;;
esac
[ -n "${2:-}" ] && FPS="$2"

export DISPLAY=${DISPLAY:-:0}
export LIBVA_DRIVER_NAME=v4l2_request
# Work over SSH too (the session's own cookie when run from the desktop).
if [ -z "${XAUTHORITY:-}" ] && [ -r "$HOME/.Xauthority" ]; then
    export XAUTHORITY="$HOME/.Xauthority"
fi

# --- GPU sanity check -------------------------------------------------------
if [ ! -e /dev/dri/renderD128 ]; then
    echo "stream-desktop: no DRM render node, reloading panfrost..." >&2
    sudo modprobe -r panfrost 2>/dev/null
    sleep 1
    sudo modprobe panfrost 2>/dev/null
    sleep 2
    if [ ! -e /dev/dri/renderD128 ]; then
        echo "stream-desktop: still no render node, streaming would use llvmpipe" >&2
    fi
fi

# --- never blank / never lock ------------------------------------------------
# The X server's default screen saver timeout (600 s) blanks the output after
# idle.  On this board the blank wakes light-locker, which locks the session;
# lightdm then switches to its greeter VT and Xorg suspends AIGLX on the
# session's VT, so every EGL client falls back to llvmpipe (~1 fps) until the VT
# is switched back.  Streamed frames are not X activity either, and presenting
# to a blanked output costs ~750 ms per swap.
# ~/.config/autostart/never-blank.desktop does the same at every login, so
# these settings are left in place when the stream ends (that is the point).
xset s off 2>/dev/null         # screen saver: never activate
xset +dpms 2>/dev/null         # keep the DPMS extension enabled (moonlight's
xset dpms 0 0 0 2>/dev/null    # DPMSForceLevel fails with BadMatch when it is
                               # disabled); zero timeouts = never blank
xset s noblank 2>/dev/null     # never blank
xset s reset 2>/dev/null       # clear a blank that is already up
xset dpms force on 2>/dev/null

# --- where is this display?  (GPU / VT / session state) ---------------------
# A client on a background VT gets no GPU: Xorg suspends AIGLX when its VT is
# not the active one, DRI3Open fails and everything silently runs on llvmpipe
# (~1 fps for a 1080p stream).  What normally pulls the VT away is lightdm's
# greeter acting as the lock screen after the screen saver blanked the output.
display_vt=$(ps -o cmd= -C Xorg 2>/dev/null | grep -- "-core ${DISPLAY} " | grep -o 'vt[0-9]*' | tr -d 'vt')
active_vt=$(cat /sys/class/tty/tty0/active 2>/dev/null | tr -d 'tty')
session_id=$(loginctl list-sessions --no-legend 2>/dev/null | awk -v u="$USER" '$3==u && $4=="seat0" {print $1; exit}')
locked=no
session_active=unknown
if [ -n "$session_id" ]; then
    locked=$(loginctl show-session "$session_id" -p LockedHint --value 2>/dev/null)
    session_active=$(loginctl show-session "$session_id" -p Active --value 2>/dev/null)
fi

# lightdm shows its greeter as the lock screen on its own VT and keeps pulling
# that VT back, so switching VTs cannot get past it (and the stream would be
# hidden behind it anyway) - detect and report it instead of fighting.
greeter_active=no
greeter_locked=no
greeter_id=
for id in $(loginctl list-sessions --no-legend 2>/dev/null | awk '$6=="greeter" {print $1}'); do
    greeter_id=$id
    [ "$(loginctl show-session "$id" -p Active --value 2>/dev/null)" = "yes" ] && greeter_active=yes
    [ "$(loginctl show-session "$id" -p LockedHint --value 2>/dev/null)" = "yes" ] && greeter_locked=yes
done

gpu_renderer() { es2_info 2>/dev/null | grep -m1 -o 'GL_RENDERER:.*'; }
renderer=$(gpu_renderer)

if [ "$locked" = "yes" ]; then
    echo "stream-desktop: WARNING: session $session_id is locked; the lock window stays above" >&2
    echo "stream-desktop:   the stream, so unlock first and run this again" >&2
elif [ "$session_active" = "no" ] && [ "$greeter_active" = "yes" ] && [ "${FORCE_VT:-0}" != "1" ]; then
    # lightdm's greeter is the active session and keeps pulling its VT back, so
    # switching VTs only wins for a minute and the stream would be yanked onto a
    # background VT (llvmpipe, ~1 fps) mid-way.  Better to say what to do.
    echo "stream-desktop: WARNING: lightdm's greeter is the active session (VT$active_vt) and your" >&2
    echo "stream-desktop:   desktop is in the background: log in at the greeter to bring it back," >&2
    echo "stream-desktop:   then run this again (FORCE_VT=1 switches the VT anyway)" >&2
elif ! echo "$renderer" | grep -q -e Mali -e Panfrost; then
    if [ -n "$display_vt" ] && [ "$display_vt" != "$active_vt" ]; then
        if [ "${FIX_VT:-1}" = "1" ] && command -v chvt >/dev/null && sudo -n true 2>/dev/null; then
            echo "stream-desktop: ${DISPLAY} is on VT$display_vt (VT$active_vt active), switching"
            sudo chvt "$display_vt"
            sleep 1
            renderer=$(gpu_renderer)
        fi
    fi
fi
if ! echo "$renderer" | grep -q -e Mali -e Panfrost; then
    echo "stream-desktop: WARNING: no GPU on ${DISPLAY} (${renderer:-no EGL renderer});" >&2
    echo "stream-desktop:   streaming will be software rendered with very low fps" >&2
    echo "stream-desktop:   no render node?  sudo modprobe -r panfrost && sudo modprobe panfrost" >&2
    if [ -n "$display_vt" ] && [ "$display_vt" != "$active_vt" ]; then
        echo "stream-desktop:   ${DISPLAY} is on VT$display_vt but VT$active_vt is active:  sudo chvt $display_vt" >&2
    fi
fi

# --- compositor off while streaming, restored on exit ----------------------
COMPOSITING_SAVED=$(xfconf-query -c xfwm4 -p /general/use_compositing 2>/dev/null)
LOG=${LOG:-$HOME/.moonlight-last.log}

restore_compositing() {
    if [ "$COMPOSITING_SAVED" = "true" ]; then
        xfconf-query -c xfwm4 -p /general/use_compositing -s true >/dev/null 2>&1
    fi
}

# Summarise the last stream from the instrumentation moonlight prints each 10
# rendered frames: avg/peak fps, the per-frame render cost and, most useful,
# whether every frame the host sent was rendered (ratio ~1 means the client is
# keeping up; >1 means it is dropping frames and the host is sending faster).
summarise_stream() {
    [ -s "$LOG" ] || return 0
    echo "stream-desktop: log: $LOG"
    awk '
        /x11: (zero-copy|fallback-download)( |$)/ {
            f = $0
            if (match(f, /delta_fps=[0-9.]+/)) { v = substr(f, RSTART + 10, RLENGTH - 10) + 0
                sum += v; n++; if (v > peak) peak = v }
            if (match(f, /submits=[0-9]+/)) { v = substr(f, RSTART + 8, RLENGTH - 8) + 0
                subs += v; windows++ }
            if (match(f, /c2=[0-9.]+ms/)) { v = substr(f, RSTART + 3, RLENGTH - 5) + 0; c2 += v; c2n++ }
            if (match(f, /c1=[0-9.]+ms/)) { v = substr(f, RSTART + 3, RLENGTH - 5) + 0
                if (v > 0) { c1 += v; c1n++ } }
        }
        END {
            if (n == 0) { print "stream-desktop: no rendered frames found in the log"; exit }
            printf "stream-desktop: rendered %d samples, avg %.1f fps, peak %.1f fps", n, sum / n, peak
            if (c2n) printf ", draw+swap %.1f ms/frame", c2 / c2n
            if (c1n) printf ", readback %.1f ms/frame", c1 / c1n
            printf "\n"
            if (windows) {
                r = (subs / windows) / 10.0
                printf "stream-desktop: host:client frame ratio %.2f (%s)\n", r,
                       (r > 1.1 ? "client is dropping frames" : (r < 0.9 ? "host sends less than we render" : "keeping up, 1 frame per sent frame"))
            }
        }' "$LOG"
}
if [ "$COMPOSITING_SAVED" = "true" ]; then
    xfconf-query -c xfwm4 -p /general/use_compositing -s false
fi

# --- (re)start the stream ---------------------------------------------------
# Ask the host to end a previous session before killing a local client:
# a client killed with SIGKILL never tears the session down, and Sunshine then
# refuses/hangs on new connections (a stale session has to time out on its own).
if pgrep -x moonlight >/dev/null 2>&1; then
    echo "stream-desktop: ending the previous session cleanly first"
    timeout 15 "$MOONLIGHT" quit "$HOST" >/dev/null 2>&1
fi
pkill -x moonlight 2>/dev/null
sleep 1
trap 'summarise_stream; restore_compositing' EXIT INT TERM HUP

echo "stream-desktop: $APP from $HOST, $RES, ${FPS} fps, $CODEC, zero-copy display"
if [ "${DRY_RUN:-0}" = "1" ]; then
    echo "stream-desktop: DRY_RUN=1, would run:"
    echo "  $MOONLIGHT -platform x11_vaapi -codec $CODEC $RES -fps $FPS -app $APP stream $HOST"
    echo "stream-desktop: (log would go to $LOG and a summary is printed on exit)"
    restore_compositing
    trap - EXIT INT TERM HUP
    exit 0
fi
"$MOONLIGHT" -platform x11_vaapi -codec "$CODEC" "$RES" -fps "$FPS" \
    ${LOCALAUDIO:+-localaudio} -app "$APP" stream "$HOST" 2>&1 | tee "$LOG"
