# H618 (Orange Pi Zero 2W) zero-copy display path

This fork branch (`h618-egl-download`) renders VAAPI frames straight from the
decoder's dma-buf instead of downloading them to system memory.  Companion
driver: [`bootlin/libva-v4l2-request`](https://github.com/bootlin/libva-v4l2-request)
with the H618 port (branch `h618-c-port`); that repository's
`docs/zero-copy-results.md` holds the measurements.  `scripts/h618-quick-start.sh`
is the launcher used on the reference board.

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

## Known issues

### Fixed vertical tear under heavy motion (recorded 2026-09-16, not fixed)

With a 1080p HEVC stream, fast horizontal motion (scrolling) shows a vertical
discontinuity - a "crack" - at a fixed horizontal position of roughly 1000 px
(reporter's estimate; a 1920-wide frame encoded with 4 slices has slice
boundaries at 480/960/1440 px).  The two sides show different frames' content
rather than wrong colours, and daily office use is unaffected.

Suspected cause: the zero-copy path hands the decoder's capture buffer to the
GPU without synchronising it against the VPU.  A surface is released once the
driver no longer needs it, so the VPU can already be writing the next frame
while the display still samples the previous one; whatever has been overwritten
then differs from the rest of the frame, which is exactly the kind of seam that
lands on a slice/tile boundary.  The CPU path (`MOONLIGHT_NO_ZC=1`) forces a
full sync and is not expected to show it.

Confirming and fixing:

* run the same content with `MOONLIGHT_NO_ZC=1`; if the tear disappears the
  hypothesis is right;
* then either keep the exported surface busy until the frame has been presented
  (release it after `eglSwapBuffers`, not before), or add explicit
  synchronisation between the VPU write and the GPU read (`DMA_BUF_IOCTL_SYNC`
  on the exported dma-buf, or a fence).

### Other known issues

* HEVC with more than one B frame per group differs from a software decoder on
  the first B frame after each IDR (hardware behaviour; H264 and HEVC with at
  most one B frame per group are byte-exact).
* The X server limits presentation, not the client: ~29 fps at 1080p with
  xfwm4's compositor and ~48-53 fps without it, independent of window size.
* A blanked output (DPMS off) or a client on a background VT (Xorg suspends
  AIGLX, DRI3Open fails) silently pushes everything onto llvmpipe at a few fps;
  the reference script detects both.
