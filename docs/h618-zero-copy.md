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
  surface width is a multiple of 32): `egl_draw_dmabuf_nv12()` builds two
  `EGLImage`s from the same dma-buf - luma as an `R8` image (width = luma pitch)
  and chroma as a `GR88` image at byte offset `objects[0].offset[1]` (width =
  pitch/2) - and the fragment shader does two `texelFetch` (`.r` = Y, `.rg` =
  U,V).  The images are cached per fd and rebuilt only on resolution change.
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
* **The presentation path is not the wall - sampling the external dma-buf is**
  (2026-09-19).  With the compositor off, an unchanged client presents at one
  vblank (`MOONLIGHT_ZC_TRIVIAL` reaches 60 fps), and `MOONLIGHT_ZC_BREAKDOWN`
  measures the import at ~0.0 ms and `eglSwapBuffers` at ~0.6 ms while the draw
  itself costs ~29 ms: the two `texelFetch` of the imported planes dominate
  (~22 ms).  A whole earlier claim that the X server cost ~19 ms/frame could not
  be reproduced.  The compositor does cost ~10 ms/frame, so keep it off while
  streaming.

## Debug switches

| Variable | Effect |
|---|---|
| `MOONLIGHT_NO_ZC=1` | force the CPU download path |
| `MOONLIGHT_NO_DRAW=1` | drop frames without rendering (decode-only rate) |
| `MOONLIGHT_ZC_TILED=1` | use the tiled de-tile shader |
| `MOONLIGHT_ZC_TRIVIAL=1` | constant-colour shader (isolates import+present) |
| `MOONLIGHT_ZC_BREAKDOWN=1` | add `glFinish()` and print import / render+finish / swap per 30 frames |
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
* A blanked output (DPMS off) or a client on a background VT (Xorg suspends
  AIGLX, DRI3Open fails) silently pushes everything onto llvmpipe at a few fps;
  the reference script detects both.

## The 1080p60 ceiling (investigated 2026-09-19)

The zero-copy path tops out at **~34-36 fps at 1080p** (`c2` ~25-29 ms).  The
limit is the GPU sampling the decoder's dma-buf in the fragment shader; it is
intrinsic to the external texture, not to X, vsync, the EGLImage import (cached,
`creates` stays at 3 for a whole session) or `eglSwapBuffers`.

Measured (1920x1080, compositor off, real stream):

| run | `c2` |
|---|---|
| no sampling (`MOONLIGHT_ZC_TRIVIAL`) | ~15 ms / 60 fps |
| one plane sampled | 19.4 ms / 51 fps |
| two planes sampled (current) | ~25-29 ms / 34-36 fps |
| GPU copy into native textures, then display | 36 ms (rejected) |

Levers that were investigated and closed:

* **Copy to GPU-native textures first** (a normal FBO pass): a net regression
  (36 ms) - the extra pass costs more than it saves.
* **Make the dma-buf cacheable**: the premise holds (cedrus uses
  `dma_alloc_coherent`, so the CPU mapping is write-combine) but the mechanism
  does not - panfrost maps *every* BO with `IOMMU_CACHE` (write-back), so the GPU
  already reads it cached.  No lever there.
* **Let the display engine scan the NV12 dma-buf directly** (hardware CSC, no GPU
  sampling): the H616 DE33 VI plane's YUV formats are deliberately disabled by
  Armbian patch `0032` because chroma upsampling needs the DE33 VI scaler, which
  is **not yet supported upstream**.  Jernej's `update DE33 support` series is
  still unmerged (as of 2026-09-19) and no scaler/RCQ enablement exists in any
  tree, including the Sipeed LonganPi-3H SDK (whose `0010` is clock plumbing and
  whose `0011` is an older bring-up that merely re-adds the broken YUV).
* **Use the SoC's G2D (2D engine) to convert NV12 -> RGB and scan that**: also
  closed.  The G2D is present (H618 Datasheet v1.1 §2.5.3) but on a mainline boot
  only its TOP wrapper responds - every sub-block register (MIXER/BLD/V0/WB/VSU)
  reads 0.  Mainline `ccu-sun50i-h616.c` also lacks the `RST_BUS_G2D` reset
  (CCU `0x63C` bit 16) that must be released.  With the reset released and all
  clocks/PLLs/gates set exactly as the vendor BSP does, the core still does not
  decode, and no public or vendor source (U-Boot, BL31, disp2, the vendor G2D
  driver) reveals the missing initialisation.  Full record:
  `superpowers/specs/2026-09-19-h618-g2d-offload-design.md` (§9).

A remaining untried idea is a GLES3.1 **compute** shader reading the external
memory with coalesced loads (potentially much better memory-level parallelism
than per-fragment `texelFetch`); it was not attempted.
