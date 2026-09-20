#!/bin/sh
# Acceptance matrix for the H618 zero-copy display path.
#   HOST=<sunshine-ip> ML=<moonlight binary> scripts/h618-accept.sh
# Runs one real stream per configuration and prints the per-frame c2
# (draw+swap). Strict cases require c2 < 16.7 ms AND final_avg_fps >= 58
# (control runs only need c2 < 16.7 ms).
set -u
export DISPLAY="${DISPLAY:-:0}"
export LIBVA_DRIVER_NAME="${LIBVA_DRIVER_NAME:-v4l2_request}"
export LIBVA_DRIVERS_PATH="${LIBVA_DRIVERS_PATH:-$HOME/Downloads/v4l2-dri}"
HOST="${HOST:?set HOST to the Sunshine host IP}"
ML="${ML:-$HOME/Downloads/moonlight-embedded/build/moonlight}"
DUR="${DUR:-22}"

[ -x "$ML" ] || { echo "accept: no executable moonlight at: $ML" >&2; exit 1; }
MLNAME=$(basename "$ML")

COMPOSITING_SAVED=$(xfconf-query -c xfwm4 -p /general/use_compositing 2>/dev/null || true)
cleanup() {
  timeout 10 "$ML" quit "$HOST" >/dev/null 2>&1
  pkill -x "$MLNAME" 2>/dev/null
  if [ "${COMPOSITING_SAVED:-}" = "true" ]; then
    xfconf-query -c xfwm4 -p /general/use_compositing -s true >/dev/null 2>&1
  fi
}
trap cleanup EXIT
trap 'exit 130' INT TERM HUP

if [ "${COMPOSITING_SAVED:-}" != "false" ]; then
  echo "accept: compositor was '${COMPOSITING_SAVED:-unknown}', disabling for this run"
  xfconf-query -c xfwm4 -p /general/use_compositing -s false >/dev/null 2>&1
fi

xset s off 2>/dev/null; xset +dpms 2>/dev/null; xset dpms 0 0 0 2>/dev/null
xset s noblank 2>/dev/null; xset dpms force on 2>/dev/null

failures=0

report() {
  name="$1"; log="$2"; strict="$3"; marker="$4"
  echo "----- $name -----"
  grep -aE "nv12 .*import|nv12 external|EGL: vsync|renderer=" "$log" | head -3
  if [ -n "$marker" ] && ! grep -aqF "$marker" "$log"; then
    echo "  FAIL(path marker missing: $marker)"
    return 1
  fi
  if [ "$marker" = "EGL: nv12 external path active" ] &&
      grep -aqE \
          -e 'EGL: nv12 external draw failed; falling back to two-plane path' \
          -e 'EGL: nv12 two-plane import' \
          -e 'EGL: native NV12 external path unavailable; using two-plane path' \
          -e 'x11: (render path -> )?(zero-copy-failed|fallback-download)([[:space:]]|$)' \
          "$log"; then
    echo "  FAIL(external path fallback detected)"
    return 1
  fi
  grep -a "x11: " "$log" | awk -v strict="$strict" '
    { has_delta=0; has_submits=0;
      for (i=1;i<=NF;i++) {
        if ($i ~ /^c2=/) { v=$i; sub(/^c2=/,"",v); sub(/ms$/,"",v); s+=v; n++ }
        if ($i ~ /^avg_fps=/) { a=$i; sub(/^avg_fps=/,"",a); last_avg_fps=a+0; qn++ }
        if ($i ~ /^delta_fps=/) { delta=$i; sub(/^delta_fps=/,"",delta); has_delta=1 }
        if ($i ~ /^submits=/) { submits=$i; sub(/^submits=/,"",submits); has_submits=1 }
      }
      if (has_delta && has_submits) { submit_sum+=submits*delta/10.0; submit_n++ }
    }
    END {
      if (n == 0) { print "  FAIL(no samples)"; exit 1 }
      ok = (s/n < 16.7);
      if (strict && (!qn || last_avg_fps < 58)) ok = 0;
      if (qn)
        printf "  samples=%d  avg_c2=%.2f ms  final_avg_fps=%.1f  %s\n", n, s/n,
               last_avg_fps, (ok ? "PASS" : "FAIL");
      else
        printf "  samples=%d  avg_c2=%.2f ms  final_avg_fps=n/a  %s\n", n, s/n,
               (ok ? "PASS" : "FAIL");
      if (submit_n) printf "  avg_submit_fps=%.1f (decoder submissions)\n", submit_sum/submit_n;
      exit (ok ? 0 : 1)
    }'
}

run() {
  name="$1"; strict="$2"; marker="$3"; shift 3
  timeout 10 "$ML" quit "$HOST" >/dev/null 2>&1
  pkill -x "$MLNAME" 2>/dev/null; sleep 1
  log="/tmp/accept_$name.log"
  env "$@" "$ML" -platform x11_vaapi -codec h265 -1080 -fps 60 \
      -localaudio -app Desktop stream "$HOST" >"$log" 2>&1 &
  pid=$!
  sleep "$DUR"
  timeout 10 "$ML" quit "$HOST" >/dev/null 2>&1
  sleep 2
  kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
  if ! report "$name" "$log" "$strict" "$marker"; then
    failures=$((failures + 1))
  fi
}

echo "compositing=$(xfconf-query -c xfwm4 -p /general/use_compositing 2>/dev/null)"
CASES="${CASES:-default trivial}"
processed=0
for case_name in $CASES; do
  processed=1
  case "$case_name" in
    default)
      run default 1 "EGL: nv12 two-plane import"
      ;;
    trivial)
      run trivial 0 "EGL: nv12 two-plane import" MOONLIGHT_ZC_TRIVIAL=1
      ;;
    external)
      run external 1 "EGL: nv12 external path active" \
          MOONLIGHT_ZC_EXTERNAL=1 MOONLIGHT_ZC_BREAKDOWN=1
      ;;
    *)
      echo "accept: unknown case '$case_name'" >&2
      failures=$((failures + 1))
      ;;
  esac
done
if [ "$processed" -eq 0 ]; then
  echo "accept: no cases selected" >&2
  failures=$((failures + 1))
fi
echo "DONE ($failures failing configuration(s))"
[ "$failures" -eq 0 ]
