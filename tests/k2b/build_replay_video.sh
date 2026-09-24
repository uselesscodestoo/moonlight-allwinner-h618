#!/bin/sh
# Link the SAME native objects used by the current integrated Moonlight build.
set -eu
cd "$(dirname "$0")/../.."
build=${1:-build/k2b-integrated}
objects="$build/CMakeFiles/moonlight.dir/src/video/k2b"
test -f "$objects/video.c.o"
header=$(find "$build/tools/k2b-runtime/managed-source" -path '*/include/vdecoder.h' -print -quit)
test -n "$header"
cedar_root=${header%/include/vdecoder.h}
mkdir -p build/k2b-tests
cc -std=c11 -O2 -Wall -Wextra -Werror -DHAVE_K2B -DTINA_LINUX_SUPPORT=0 \
    -I"$cedar_root/include" -I"$cedar_root/base/include" -I"$cedar_root/vdecoder/include" \
    -Ithird_party/moonlight-common-c/src \
    $(pkg-config --cflags libavformat libavcodec libavutil) \
    tests/k2b/replay_video.c \
    "$objects/video.c.o" "$objects/access_unit.c.o" "$objects/input_queue.c.o" \
    "$objects/disp_presenter.c.o" "$objects/disp_config.c.o" \
    "$build/tools/k2b-runtime/libk2b_cedar_runtime.a" \
    -Wl,--wrap=k2b_disp_present -Wl,--wrap=k2b_cedar_runtime_load -pthread -ldl \
    $(pkg-config --libs libavformat libavcodec libavutil) \
    -o build/k2b-tests/replay_video
