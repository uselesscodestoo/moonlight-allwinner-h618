#!/bin/sh
# Native Longan K2B build; keep Cedar libraries private to this process.
set -eu
if [ "$#" -lt 1 ]; then
    echo "Usage: sudo sh tools/k2b-stream.sh SUNSHINE_IP [moonlight options]" >&2
    exit 2
fi
host=$1
shift
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
if [ "$(id -u)" -ne 0 ]; then
    echo "Run with sudo: the vendor display, VPU and input nodes require root." >&2
    exit 1
fi
if [ ! -c /dev/cedar_test_heap ]; then
    echo "Load the matching cedar_test_heap.ko first (see docs/k2b-live-stream.md)." >&2
    exit 1
fi
exec "$root/tools/k2b-runtime/run-private.sh" \
    "$root/build/k2b-integrated/tools/k2b-runtime/runtime" \
    "$root/build/k2b-integrated/moonlight" stream \
    -platform k2b -app Desktop -1080 -fps 60 -codec h264 -bitrate 15000 \
    -audio hw:1,0 \
    -mapping "$root/third_party/SDL_GameControllerDB/gamecontrollerdb.txt" \
    -keydir "$root/build/k2b-pairing" "$@" "$host"
