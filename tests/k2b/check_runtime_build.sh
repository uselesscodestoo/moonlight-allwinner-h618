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
# Exercise the actual shell entrypoints without faking the host architecture.
# These directories are valid filesystem paths but have ld.so syntax in them.
path_fixtures=$(mktemp -d "$build/launch-paths.XXXXXX")
path_failures=0
reject_launch_path() {
    path_log=$1
    shift
    if "$@" >"$path_log" 2>&1; then
        echo "FAIL: unsafe runtime path unexpectedly accepted; log: $path_log" >&2
        path_failures=$((path_failures + 1))
    elif ! grep -Fxq 'FAIL: runtime path must not contain whitespace, colon, semicolon, or dollar sign' "$path_log"; then
        cat "$path_log" >&2
        echo "FAIL: runtime path did not reach its rejection gate; log: $path_log" >&2
        path_failures=$((path_failures + 1))
    fi
}
path_case=0
for component in 'semi;colon' 'token$ORIGIN' 'token${LIB}' 'token$PLATFORM'; do
    path_case=$((path_case + 1))
    path_fixture="$path_fixtures/$component"
    mkdir -- "$path_fixture"
    reject_launch_path "$path_fixtures/launcher-$path_case.log" \
        sh "$root/tools/k2b-runtime/run-private.sh" "$path_fixture" /bin/true
    reject_launch_path "$path_fixtures/load-check-$path_case.log" \
        sh "$root/tests/k2b/check_runtime_load.sh" "$root" "$path_fixture"
done
[ "$path_failures" -eq 0 ] || fail "$path_failures runtime path rejection checks failed"
echo 'PASS: both shell entrypoints reject ld.so separators and dynamic-token paths'
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
# The preload stays independent of every Cedar library, and does not enter the
# original closure. These are static ELF checks; no produced file is executed.
compat="$runtime/libk2b_cedar54_compat.so"
[ -f "$compat" ] || fail 'missing compat preload'
"$readelf" -h "$compat" | grep -Eq 'Class: +ELF64' || fail 'compat is not ELF64'
"$readelf" -h "$compat" | grep -Eq 'Machine: +AArch64' || fail 'compat is not ARM64'
"$readelf" -d "$compat" | grep -F '(SONAME)' | grep -Fq '[libk2b_cedar54_compat.so]' ||
    fail 'compat SONAME'
"$readelf" --dyn-syms --wide "$compat" |
    awk '$4 == "FUNC" && $5 == "GLOBAL" && $6 == "DEFAULT" && $7 != "UND" && $8 == "ioctl" { found=1 } END { exit !found }' ||
    fail 'compat does not export public ioctl'
if "$readelf" --dyn-syms --wide "$compat" | grep -q 'k2b_cedar54_ioctl_dispatch'; then
    fail 'compat helper is dynamically exported'
fi
"$readelf" -l --wide "$compat" | grep 'GNU_STACK' | grep -Eq ' RW +0x' ||
    fail 'compat stack is not non-executable'
preload_test="$runtime/k2b_cedar54_preload_test"
[ -f "$preload_test" ] || fail 'missing preload ABI test'
"$readelf" -h "$preload_test" | grep -Eq 'Machine: +AArch64' || fail 'preload test architecture'
for file in "$compat" "$preload_test"; do
    "$readelf" -d "$file" | awk '/\(NEEDED\)/ { print $NF }' |
        while read -r needed; do
            case "$needed" in
                '[libc.so.6]'|'[libdl.so.2]'|'[ld-linux-aarch64.so.1]') ;;
                *) fail "$file has unexpected dependency: $needed" ;;
            esac
        done
done
load_check="$runtime/k2b_runtime_load_check"
[ -f "$load_check" ] || fail 'missing runtime load-check ELF'
"$readelf" -h "$load_check" | grep -Eq 'Class: +ELF64' || fail 'load check is not ELF64'
"$readelf" -h "$load_check" | grep -Eq 'Machine: +AArch64' || fail 'load check is not ARM64'
"$readelf" -d "$load_check" | awk '/\(NEEDED\)/ { print $NF }' |
    while read -r needed; do
        case "$needed" in
            '[libc.so.6]'|'[libdl.so.2]'|'[libpthread.so.0]'|'[ld-linux-aarch64.so.1]') ;;
            *) fail "load check has unexpected dependency: $needed" ;;
        esac
    done
for lib in cdc_base MemAdapter sbm fbm vdecoder VE videoengine awh264 vdecVcs; do
    if "$readelf" -d "$runtime/lib$lib.so" | grep -F '(NEEDED)' |
        grep -Fq '[libk2b_cedar54_compat.so]'; then
        fail "preload was linked into $lib"
    fi
done
while read -r expected relative; do
    actual=$(sha256sum "$runtime/${relative##*/}")
    [ "${actual%% *}" = "$expected" ] || fail "blob hash: $relative"
done < "$root/docs/k2b-cedarc-blobs.sha256"
link_check="$runtime/k2b_runtime_link_check"
[ -f "$link_check" ] || fail "missing complete link-check ELF"
if "$readelf" -d "$link_check" | grep -F '(NEEDED)' | grep -Fq '[libk2b_cedar54_compat.so]'; then
    fail 'preload was linked into the complete link check'
fi
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
echo 'PASS: nine private ARM64 libraries, independent compat preload and runtime loader, strict complete link, ELF checks, and input rejection checks (no target execution)'
