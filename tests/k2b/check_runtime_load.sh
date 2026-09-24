#!/bin/sh
# Explicit native, non-root, pure-load check. Never create a decoder/session.
set -eu
# The caller must start a fresh sanitized shell: unsetting cannot undo a preload
# already mapped into this shell. Clear these before invoking any child tools.
unset LD_AUDIT LD_PRELOAD LD_LIBRARY_PATH CEDAR_K2B_KERNEL54_COMPAT K2B_CEDAR_RUNTIME_DIR
export LC_ALL=C
fail() { echo "FAIL: $*${logs:+; logs: $logs}" >&2; exit 1; }
[ "$#" -eq 2 ] || { echo "usage: $0 PROJECT_ROOT RUNTIME_DIR" >&2; exit 2; }
root=$(realpath -e -- "$1")
runtime=$(realpath -e -- "$2")
[ -d "$root" ] && [ -d "$runtime" ] || fail 'project and runtime must be directories'
# Match the launcher's literal-path restriction before any test execution.
case "$runtime" in
    *[[:space:]:]*|*';'*|*'$'*)
        fail 'runtime path must not contain whitespace, colon, semicolon, or dollar sign' ;;
esac
[ "$(uname -s)" = Linux ] && [ "$(uname -m)" = aarch64 ] ||
    fail 'requires native Linux AArch64'
[ "$(id -u)" -ne 0 ] || fail 'requires a non-root user'
command -v strace >/dev/null 2>&1 || fail 'strace is required'
launcher="$root/tools/k2b-runtime/run-private.sh"
load_check="$runtime/k2b_runtime_load_check"
[ -f "$launcher" ] && [ -r "$launcher" ] || fail "missing launcher: $launcher"
[ -f "$load_check" ] && [ -x "$load_check" ] || fail "missing executable: $load_check"
mkdir -p -- "$root/build"
logs=$(mktemp -d "$root/build/k2b-runtime-load.XXXXXX")
echo "Runtime load logs: $logs"
audit_trace() {
    trace=$1
    [ -s "$trace" ] || fail "empty trace: $trace"
    grep -Eq 'execve\(' "$trace" || fail "no execution sample: $trace"
    grep -Eq 'open(at)?\(' "$trace" || fail "no library-open sample: $trace"
    if grep -Eq 'ioctl\(' "$trace"; then
        fail "unexpected ioctl during pure loading: $trace"
    fi
    if grep -E 'open(at)?\(' "$trace" |
        grep -Eq '"/dev/[^"[:space:]]*(cedar|disp|heap|ion|dri|video)'; then
        fail "device open during pure loading: $trace"
    fi
}
# Existing valid libraries ensure this tests the environment gate, not missing
# files. File realpath/stat checks must not cause any Cedar library open.
if strace -f -qq -s 4096 -e trace=execve,open,openat,ioctl -o "$logs/negative.trace" \
    "$load_check" "$runtime" >"$logs/negative.stdout" 2>"$logs/negative.stderr"; then
    fail 'missing private environment unexpectedly succeeded'
fi
grep -Fq 'FAIL: runtime load (first): errno=22 ' "$logs/negative.stderr" ||
    fail 'negative case did not report EINVAL from the first loader call'
grep -Fq 'ready=0 error=22 detail=environment LD_LIBRARY_PATH:' "$logs/negative.stderr" ||
    fail 'negative case did not reject the exact LD_LIBRARY_PATH gate'
if grep -Eq 'PASS:|typed H264 hardware registration' "$logs/negative.stdout" "$logs/negative.stderr"; then
    fail 'negative case reported loading or registration success'
fi
audit_trace "$logs/negative.trace"
if grep -E 'open(at)?\(' "$logs/negative.trace" |
    grep -Eq 'lib(cdc_base|MemAdapter|sbm|fbm|vdecoder|VE|videoengine|awh264|vdecVcs|awh265|k2b_cedar54_compat)\.so'; then
    fail 'negative case opened a Cedar library or compat preload'
fi
echo 'PASS: missing private environment rejected before Cedar library loading'
if ! strace -f -qq -s 4096 -e trace=execve,open,openat,ioctl -o "$logs/positive.trace" \
    sh "$launcher" "$runtime" "$load_check" "$runtime" \
    >"$logs/positive.stdout" 2>"$logs/positive.stderr"; then
    fail 'private runtime load failed'
fi
audit_trace "$logs/positive.trace"
grep -Fxq 'PASS: runtime ready=1 error=0; repeat load returned same API table' "$logs/positive.stdout" ||
    fail 'missing successful repeated-load result'
grep -Fxq 'PASS: typed H264 hardware registration via VDecoderRegister(format=H264, name=h264, bIsSoft=0)' "$logs/positive.stdout" ||
    fail 'missing typed hardware registration result'
grep -Fxq 'PASS: typed H265 hardware registration via VDecoderRegister(format=H265, name=h265, bIsSoft=0)' "$logs/positive.stdout" ||
    fail 'missing typed HEVC hardware registration result'
grep -Fxq 'PASS: memory untouched active=0 references=0 allocations=0 live_bytes=0 peak_bytes=0 pinned=0 quarantined=0 error=0' "$logs/positive.stdout" ||
    fail 'missing untouched memory result'
for lib in cdc_base MemAdapter sbm fbm vdecoder VE videoengine awh264 vdecVcs awh265 k2b_cedar54_compat; do
    grep -E 'open(at)?\(' "$logs/positive.trace" | grep -F "\"$runtime/lib$lib.so\"" |
        grep -Eq '= [0-9]+$' || fail "no successful private library-open sample: lib$lib.so"
done
echo "PASS: native private load, typed H264/H265 hardware registration, untouched memory, no device opens or ioctls; logs: $logs"
