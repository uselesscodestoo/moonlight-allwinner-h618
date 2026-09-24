#!/bin/sh
# Start from a fresh sanitized process: unsetting cannot unload a library that
# was already preloaded into this shell. Clear these three loader variables
# before starting any child tool.
set -eu
unset LD_AUDIT LD_PRELOAD LD_LIBRARY_PATH
export LC_ALL=C
fail() { echo "FAIL: $*" >&2; exit 1; }
[ "$#" -ge 2 ] || { echo "usage: $0 RUNTIME_DIR EXECUTABLE [ARGS...]" >&2; exit 2; }
runtime=$(realpath -e -- "$1")
executable=$(realpath -e -- "$2")
shift 2
[ -d "$runtime" ] || fail "runtime is not a directory: $runtime"
# ld.so splits library paths at ':' and ';' and expands dollar tokens.
case "$runtime" in
    *[[:space:]:]*|*';'*|*'$'*)
        fail 'runtime path must not contain whitespace, colon, semicolon, or dollar sign' ;;
esac
[ "$(uname -s)" = Linux ] && [ "$(uname -m)" = aarch64 ] ||
    fail 'requires native Linux AArch64'
[ -f "$executable" ] && [ -x "$executable" ] || fail "not an executable file: $executable"
for lib in cdc_base MemAdapter sbm fbm vdecoder VE videoengine awh264 vdecVcs k2b_cedar54_compat; do
    [ -f "$runtime/lib$lib.so" ] && [ -r "$runtime/lib$lib.so" ] ||
        fail "missing readable regular library: $runtime/lib$lib.so"
done
script=$(realpath -e -- "$0")
root=$(CDPATH= cd -- "$(dirname -- "$script")/../.." && pwd -P)
manifest="$root/docs/k2b-cedarc-blobs.sha256"
[ -f "$manifest" ] && [ -r "$manifest" ] || fail "missing blob manifest: $manifest"
[ "$(wc -l < "$manifest")" -eq 4 ] || fail 'blob manifest must contain exactly four entries'
# Preserve the fixed expected hashes, removing only the pinned archive prefix.
sed 's@  library/aarch64-none-linux-gnu/@  @' "$manifest" |
    (CDPATH= cd -- "$runtime" && sha256sum -c -) || fail 'private blob SHA256 validation failed'
exec env -u LD_AUDIT -u LD_PRELOAD -u LD_LIBRARY_PATH \
    CEDAR_K2B_KERNEL54_COMPAT=1 LD_LIBRARY_PATH="$runtime" \
    LD_PRELOAD="$runtime/libk2b_cedar54_compat.so" K2B_CEDAR_RUNTIME_DIR="$runtime" \
    "$executable" "$@"
