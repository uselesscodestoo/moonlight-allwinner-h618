#!/bin/sh
# Acceptance matrix for the H618 zero-copy display path.
#   HOST=<sunshine-ip> ML=<moonlight binary> scripts/h618-accept.sh
# Runs one real stream per configuration and prints the per-frame c2
# (draw+swap). Exits non-zero unless the "default" run reaches c2 < 16.7 ms
# AND avg_fps >= 58 (control runs only need c2 < 16.7 ms).
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
  name="$1"; log="$2"; strict="$3"
  echo "----- $name -----"
  grep -aE "nv12 .*import|EGL: vsync|renderer=" "$log" | head -2
  grep -a "x11: " "$log" | awk -v strict="$strict" '
    { for (i=1;i<=NF;i++) if ($i ~ /^c2=/)  { v=$i; sub(/^c2=/,"",v);  sub(/ms$/,"",v); s+=v; n++ }
      for (i=1;i<=NF;i++) if ($i ~ /^delta_fps=/) { w=$i; sub(/^delta_fps=/,"",w); fs+=w; fn++ }
      for (i=1;i<=NF;i++) if ($i ~ /^avg_fps=/) { a=$i; sub(/^avg_fps=/,"",a); qs+=a; qn++ } }
    END {
      if (n == 0) { print "  FAIL(no samples)"; exit 1 }
      ok = (s/n < 16.7);
      if (strict && (!qn || qs/qn < 58)) ok = 0;
      if (qn)
        printf "  samples=%d  avg_c2=%.2f ms  avg_fps=%.1f  %s\n", n, s/n,
               qs/qn, (ok ? "PASS" : "FAIL");
      else
        printf "  samples=%d  avg_c2=%.2f ms  avg_fps=n/a  %s\n", n, s/n,
               (ok ? "PASS" : "FAIL");
      if (fn) printf "  avg_delta_fps=%.1f (host rate)\n", fs/fn;
      exit (ok ? 0 : 1)
    }'
}

run() {
  name="$1"; strict="$2"; shift 2
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
  if ! report "$name" "$log" "$strict"; then
    failures=$((failures + 1))
  fi
}

echo "compositing=$(xfconf-query -c xfwm4 -p /general/use_compositing 2>/dev/null)"
run default 1
run trivial 0 MOONLIGHT_ZC_TRIVIAL=1
echo "DONE ($failures failing configuration(s))"
[ "$failures" -eq 0 ]
