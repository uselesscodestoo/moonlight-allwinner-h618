#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
root=$(pwd -P)
cc -c -O2 -Wall -Wextra -Werror tests/k2b/video_loss_inject.c -o build/k2b-tests/video_loss_inject.o
cd build/k2b-integrated
# Append the test object and a final output option; the normal binary is not changed.
link=$(cat CMakeFiles/moonlight.dir/link.txt)
sh -c "$link '$root/build/k2b-tests/video_loss_inject.o' -pthread -ldl -o '$root/build/k2b-tests/moonlight-video-loss'"
nm -D ../k2b-tests/moonlight-video-loss | grep ' T recvfrom$'
nm -D moonlight | grep ' T setsockopt$'
