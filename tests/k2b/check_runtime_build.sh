#!/bin/sh
# Local cross-build/ELF checks only. Never execute any produced ARM64 file.
set -eu
export LC_ALL=C
if [ "$#" -ne 3 ]; then
    echo "usage: $0 ARCHIVE VENDOR_HEADERS BUILD_DIR" >&2
    exit 2
fi
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
archive=$(realpath -- "$1")
headers=$(realpath -- "$2")
mkdir -p -- "$3"
build=$(CDPATH= cd -- "$3" && pwd)
cross_cc=${CROSS_CC:-aarch64-linux-gnu-gcc}
readelf=${READELF:-readelf}
fail() { echo "FAIL: $*" >&2; exit 1; }
configure() {
    configure_dir=$1
    shift
    cmake -S "$root/tools/k2b-runtime" -B "$configure_dir" \
        -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
        "-DCMAKE_C_COMPILER=$cross_cc" \
        "-DK2B_CEDARC_ARCHIVE=$archive" \
        "-DK2B_VENDOR_CEDAR_HEADERS=$headers" "$@"
}
configure "$build/positive"
cmake --build "$build/positive" --parallel 4
runtime="$build/positive/runtime"
assert_export() {
    "$readelf" --dyn-syms --wide "$runtime/$1" |
        awk -v symbol="$2" '$7 != "UND" && $8 == symbol { found=1 } END { exit !found }' ||
        fail "$1 does not export $2"
}
for lib in cdc_base MemAdapter sbm fbm vdecoder VE videoengine awh264 vdecVcs; do
    file="$runtime/lib$lib.so"
    [ -f "$file" ] || fail "missing $file"
    "$readelf" -h "$file" | grep -Eq 'Class: +ELF64' || fail "$lib is not ELF64"
    "$readelf" -h "$file" | grep -Eq 'Machine: +AArch64' || fail "$lib is not ARM64"
    "$readelf" -d "$file" | grep -F '(SONAME)' | grep -Fq "[lib$lib.so]" ||
        fail "$lib SONAME"
done
for lib in cdc_base MemAdapter sbm fbm vdecoder; do
    "$readelf" -d "$runtime/lib$lib.so" | grep -E '\((RUNPATH|RPATH)\)' |
        grep -Fq '$ORIGIN' || fail "$lib missing private origin RPATH"
done
assert_export libcdc_base.so CdcMessageQueueCreate
assert_export libMemAdapter.so MemAdapterGetOpsS
assert_export libMemAdapter.so k2b_cedar_memory_begin
assert_export libMemAdapter.so k2b_cedar_memory_end
assert_export libsbm.so SbmStreamInit
assert_export libfbm.so FbmCreate
for symbol in CreateVideoDecoder InitializeVideoDecoder DestroyVideoDecoder \
    RequestVideoStreamBuffer SubmitVideoStreamData DecodeVideoStream RequestPicture ReturnPicture; do
    assert_export libvdecoder.so "$symbol"
done
assert_export libVE.so GetVeOpsS
assert_export libvideoengine.so VideoEngineCreate
assert_export libawh264.so CedarPluginVDInit
assert_export libvdecVcs.so vcsCreate
while read -r expected relative; do
    actual=$(sha256sum "$runtime/${relative##*/}")
    [ "${actual%% *}" = "$expected" ] || fail "blob hash: $relative"
done < "$root/docs/k2b-cedarc-blobs.sha256"
link_check="$runtime/k2b_runtime_link_check"
[ -f "$link_check" ] || fail "missing complete link-check ELF"
"$readelf" -h "$link_check" | grep -Eq 'Machine: +AArch64' || fail "link check architecture"
for lib in cdc_base MemAdapter sbm fbm vdecoder VE videoengine awh264 vdecVcs; do
    "$readelf" -d "$link_check" | grep -F '(NEEDED)' | grep -Fq "[lib$lib.so]" ||
        fail "link check omitted $lib"
done
# Each invocation keeps its own fixtures/logs and never removes an external input.
negative=$(mktemp -d "$build/rejections.XXXXXX")
reject() {
    name=$1
    diagnostic=$2
    shift 2
    if configure "$negative/$name" "$@" >"$negative/$name.log" 2>&1; then
        fail "$name unexpectedly configured"
    fi
    grep -Fq "$diagnostic" "$negative/$name.log" || {
        cat "$negative/$name.log" >&2
        fail "$name did not report: $diagnostic"
    }
    echo "PASS: rejects $name"
}
reject missing-archive 'K2B CedarC archive is missing' "-DK2B_CEDARC_ARCHIVE=$negative/absent.tar.gz"
reject bad-archive 'K2B CedarC archive SHA256 mismatch' "-DK2B_CEDARC_ARCHIVE=$root/tests/k2b/check_cedar_abi.c"
reject missing-header 'K2B vendor cedar_ve.h is missing' "-DK2B_VENDOR_CEDAR_HEADERS=$negative/no-headers"
mkdir "$negative/bad-headers"
cp "$root/tests/k2b/check_cedar_abi.c" "$negative/bad-headers/cedar_ve.h"
reject bad-header 'K2B vendor cedar_ve.h SHA256 mismatch' "-DK2B_VENDOR_CEDAR_HEADERS=$negative/bad-headers"
reject native-compiler 'K2B runtime requires a Linux AArch64 64-bit compiler' "-DCMAKE_C_COMPILER=${NATIVE_CC:-cc}"
# Reconfigure an already successful build: cached checks cannot hide absent inputs.
if configure "$build/positive" "-DK2B_CEDARC_ARCHIVE=$negative/absent.tar.gz" >"$negative/cached-archive.log" 2>&1; then
    fail 'cached configure accepted missing archive'
fi
grep -Fq 'K2B CedarC archive is missing' "$negative/cached-archive.log" || fail 'cached archive diagnostic'
if configure "$build/positive" "-DK2B_VENDOR_CEDAR_HEADERS=$negative/no-headers" >"$negative/cached-header.log" 2>&1; then
    fail 'cached configure accepted missing vendor header'
fi
grep -Fq 'K2B vendor cedar_ve.h is missing' "$negative/cached-header.log" || fail 'cached header diagnostic'
configure "$build/positive"
cmake --build "$build/positive" --parallel 4
echo 'PASS: nine private ARM64 libraries, strict complete link, ELF checks, and input rejection checks (no target execution)'
