#!/bin/sh
# Explicit native ABI test: pipes only, no Cedar runtime or device ioctls.
set -eu
unset LD_PRELOAD LD_AUDIT LD_LIBRARY_PATH
export LC_ALL=C
fail() { echo "FAIL: $*" >&2; exit 1; }
[ "$#" -eq 2 ] || { echo "usage: $0 LIB TEST" >&2; exit 2; }
[ "$(uname -s)" = Linux ] && [ "$(uname -m)" = aarch64 ] ||
    fail 'requires native Linux AArch64'
lib=$(realpath -- "$1")
test=$(realpath -- "$2")
[ -f "$lib" ] || fail "missing library: $lib"
[ -x "$test" ] || fail "missing executable test: $test"
unset CEDAR_K2B_KERNEL54_COMPAT
"$test" --baseline
CEDAR_K2B_KERNEL54_COMPAT=0 LD_PRELOAD="$lib" "$test" "$lib"
CEDAR_K2B_KERNEL54_COMPAT=1 LD_PRELOAD="$lib" "$test" "$lib"
echo 'PASS: native baseline and both compat opt-in states (pipes only)'
