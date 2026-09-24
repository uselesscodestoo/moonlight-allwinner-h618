#!/bin/sh
# Explicit diagnostic, never used by the normal launcher.
set -eu
test "$#" -eq 2 || { echo 'Usage: sudo sh tests/k2b/run_video_loss.sh HOST short|partial' >&2; exit 2; }
host=$1
case "$2" in short|partial) export K2B_TEST_VIDEO_LOSS=$2;; *) exit 2;; esac
cd "$(dirname "$0")/../.."
root=$(pwd -P)
runtime="$root/build/k2b-integrated/tools/k2b-runtime/runtime"
export PULSE_SERVER="unix:/run/user/${SUDO_UID:?run with sudo}/pulse/native"
export PULSE_COOKIE=/home/kickpi/.config/pulse/cookie
exec sh tools/k2b-runtime/run-private.sh "$runtime" \
  "$root/build/k2b-tests/moonlight-video-loss" stream -platform k2b -app Desktop \
  -1080 -fps 60 -codec h264 -bitrate 15000 -audio pulse \
  -mapping "$root/third_party/SDL_GameControllerDB/gamecontrollerdb.txt" \
  -keydir "$root/build/k2b-pairing" "$host"
