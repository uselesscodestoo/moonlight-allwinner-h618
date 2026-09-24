#!/bin/sh
# Run from the repository root. Only compiles; never executes target binaries.
set -eu
if [ "$#" -ne 2 ]; then
    echo "usage: sh tests/k2b/check_cedar_abi.sh CEDARC_SOURCE BUILD_DIR" >&2
    exit 2
fi
cedarc_source=$1
abi_build=$2
abi_cc=${CROSS_CC:-aarch64-linux-gnu-gcc}
test -f "$cedarc_source/include/sc_interface.h"
test -f tests/k2b/check_cedar_abi.c
mkdir -p "$abi_build"

compile_abi() {
    "$abi_cc" -std=c99 -Wall -Wextra -Werror "$@" \
        -I"$cedarc_source/include" -I"$cedarc_source/base/include" \
        -I"$cedarc_source/base/include/gralloc_metadata" \
        tests/k2b/check_cedar_abi.c -c -o "$abi_build/check.o"
}

compile_abi -DTINA_LINUX_SUPPORT=0 >"$abi_build/positive.log" 2>&1
echo "PASS: pinned Linux AArch64 ABI compiles"

if compile_abi -DTINA_LINUX_SUPPORT=1 >"$abi_build/wrong-tina.log" 2>&1; then
    echo "FAIL: wrong TINA ABI was accepted" >&2
    exit 1
fi
if ! grep -q 'K2B CedarC ABI requires TINA_LINUX_SUPPORT=0' "$abi_build/wrong-tina.log"; then
    echo "FAIL: wrong TINA rejection was not the expected gate" >&2
    exit 1
fi
echo "PASS: wrong TINA ABI rejected"

if compile_abi -DTINA_LINUX_SUPPORT=0 \
    -Itests/k2b/abi-fixtures/swapped-memops >"$abi_build/swapped-memops.log" 2>&1; then
    echo "FAIL: swapped ScMemOpsS slots were accepted" >&2
    exit 1
fi
if ! grep -q 'k2b_memops_slot_' "$abi_build/swapped-memops.log"; then
    echo "FAIL: swapped slots rejection was not a memory ABI assertion" >&2
    exit 1
fi
echo "PASS: swapped ScMemOpsS slots rejected"
