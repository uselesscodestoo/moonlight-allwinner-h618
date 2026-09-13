# H618 (Orange Pi Zero 2W) zero-copy display path

This fork branch (`h618-egl-download`) renders VAAPI frames straight from the
decoder's dma-buf instead of downloading them to system memory.  Companion
driver: `libva-v4l2-request` branch `h618-c-port` (that repo's
`docs/zero-copy-results.md` has the measurements, and
`~/Downloads/handoff/RESULTS.md` plus `handoff/verify.sh` on the board have the
operational view).

## Why

The cedrus VPU decodes 1080p60 content at >100 fps, but the old CPU path paid
~55 ms per 1080p frame just reading the capture buffer back (the mmap is
uncached, ~65 MB/s), which capped streaming at ~15 fps with ~74 % CPU.
The GPU can consume the same buffer for ~2-4 ms per frame.

## Paths

`src/video/x11.c` picks one of three renderers per frame, from the descriptor
returned by `vaExportSurfaceHandle(DRM_PRIME_2)`:

* **Linear NV12** (`objects[0].drm_format_modifier == 0`, produced when the
  surface width is a multiple of 32): `egl_draw_dmabuf_nv12()` imports the whole
  dma-buf once as an `R8` image (`EGL_WIDTH = luma pitch`) and the fragment
  shader fetches Y at `(x, y)` and interleaved UV at row `uv_offset / pitch`.
  No de-tiling arithmetic at all.
* **Tiled NV12** (`SUNXI_TILED_NV12`): `egl_draw_dmabuf()` imports the same way
  and de-tiles per fragment (32x32-byte tiles, left to right, 32-row bands).
  Correct but far too slow on Mali-G31 (~200 ms/frame at 720p), so it is opt-in
  via `MOONLIGHT_ZC_TILED=1`.
* **CPU download** (`av_hwframe_transfer_data` + `sws_scale` + `egl_draw`), used
  when neither import works or with `MOONLIGHT_NO_ZC=1`.

Two further fixes live here: the viewport used to come from the stream
resolution instead of `eglQuerySurface()` (a fullscreen 1080p window showing a
720p stream sampled outside the frame), and the stream's own colour metadata now
drives the YCbCr->RGB coefficients (`egl_set_color_params()`); Sunshine tags
limited-range BT.601, which the shaders previously ignored.

## Traps worth remembering

* **A blanked output costs ~750 ms per swap** (glxgears 1920x1080 drops from
  75 fps to 1 fps).  `x11_setup` now disables DPMS *and* forces the output on;
  `MOONLIGHT_NO_VSYNC=1` is only for benchmarking.
* **A client on a background VT gets llvmpipe**: Xorg suspends AIGLX when its VT
  is not active, so DRI3Open fails and everything is software rendered.  On this
  board lightdm's greeter (the lock screen) is what pulls the VT away after the
  screen saver blanked the session.  `~/stream-desktop.sh` detects this and says
  what to do.
* **The X server is the presentation ceiling, not the client**: a fullscreen
  1080p client caps at ~29 fps with xfwm4's compositor and ~48-53 fps without
  it (measured with `glxgears` and `tools/egl_bench.c`); the cost is
  size-independent, i.e. it is the server's per-frame work.

## Debug switches

| Variable | Effect |
|---|---|
| `MOONLIGHT_NO_ZC=1` | force the CPU download path |
| `MOONLIGHT_NO_DRAW=1` | drop frames without rendering (decode-only rate) |
| `MOONLIGHT_ZC_TILED=1` | use the tiled de-tile shader |
| `MOONLIGHT_ZC_TRIVIAL=1` | constant-colour shader (isolates import+present) |
| `MOONLIGHT_COLOR_SELFTEST=1` | run the YCbCr->RGB self-test at startup |
| `MOONLIGHT_NO_VSYNC=1` | `eglSwapInterval(0)` |
| `V4L2_CAPTURE_FORMAT=nv12\|tiled` | driver-side capture format override |
| `V4L2_DEBUG_VERBOSE/TIMING/H265/RAW` | libva driver tracing |

`x11.c` prints one line per 10 rendered frames with the achieved fps, the
decode units the host actually sent, and the per-stage cost, which is how the
"is the client keeping up?" question is answered.
