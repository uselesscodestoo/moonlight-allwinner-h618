#!/bin/sh
mode=$1
shift
printf 'ARG=<%s>\n' "$@"
printf 'PULSE=<%s>|<%s>\n' "${PULSE_SERVER:-}" "${PULSE_COOKIE:-}"
echo WORKER_READY
case "$mode" in
    normal) exit 0 ;;
    error) exit 7 ;;
    term) kill -TERM "$$" ;;
    kill) kill -KILL "$$" ;;
    execfail) exec /not-existing-k2b-test-program ;;
    wait) exec sleep 60 ;;
    *) exit 2 ;;
esac
