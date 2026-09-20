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

### Native NV12 external sampling (tested 2026-09-20)

**CORRECT BUT TOO SLOW.** The opt-in native NV12 external texture path in
`27672b8` passed the real-stream visual gate, but the end-to-end performance
gate was not repeatably met. This classification does not isolate the cause of
the slowdown: draw/swap cost passed in both corrected runs, but one session's
final rendered rate still missed 58 fps despite near-60 decoder submissions.
Keep it available with `MOONLIGHT_ZC_EXTERNAL=1`; the next action is the
compute-shader plan. No compute
implementation was attempted as part of this gate.

The authoritative evidence combines the retained final-matrix baseline logs
and a fresh two-run external block after correcting the reporting defect
described below. All four were evaluated with the final corrected `report()`
function. `final_avg_fps` is the last cumulative rendered-frame rate in each
log; `avg_submit_fps` is the mean estimate from paired decoder-submission counts
and reporting-window rates.

| case (corrected report) | samples | `avg_c2` | `final_avg_fps` | `avg_submit_fps` | exit | result |
|---|---|---|---|---|---|---|
| default | 116 | 26.93 ms | 36.2 | 60.1 | 1 | FAIL |
| trivial | 191 | 11.71 ms | 60.0 | 60.1 | 0 | PASS |
| external 1 | 190 | 14.04 ms | 59.9 | 60.1 | 0 | PASS |
| external 2 | 165 | 13.65 ms | 57.9 | 60.2 | 1 | FAIL (`final_avg_fps < 58`) |

Default and trivial had the required two-plane marker; both external sessions
had the exact `EGL: nv12 external path active` marker and no fallback. All logs
had 1920x1080 frame samples. The final script was deployed, passed remote
`sh -n`, and reproduced these outputs from the saved logs without launching
another stream. Its `final_avg_fps` values matched the actual log tails.
The tail records independently confirmed `frame=1900 avg_fps=59.9
delta_fps=60.1 submits=11 c2=12.7ms` and `frame=1650 avg_fps=57.9
delta_fps=58.8 submits=10 c2=14.7ms`. The three-buffer image cache remained
stable throughout both sessions:

| corrected run | first -> last image/import | first -> last render + finish | first -> last swap | creates | first -> last hits |
|---|---|---|---|---|---|
| external 1 | 0.46 -> 0.18 ms | 13.67 -> 12.20 ms | 1.04 -> 0.31 ms | 3 | 27 -> 1887 |
| external 2 | 0.39 -> 0.47 ms | 13.05 -> 12.64 ms | 0.66 -> 0.98 ms | 3 | 27 -> 1647 |

The earlier summaries below are retained as diagnostic history and are
superseded for acceptance by this corrected block. No renderer code or
thresholds were changed during these measurements, and no implementation revert
was required.

All live tests used HEVC 1920x1080 at a requested 60 fps from Sunshine
`172.31.30.147`, with the scratch client under
`scy@172.30.71.3:~/Downloads/probe/moonlight-dev`. The production checkout under
`~/Downloads/moonlight-embedded` was not used. Before deployment, `pgrep` found
no Moonlight process, the renderer was `Mali-G31 MC1 (Panfrost)`, and
`/general/use_compositing` was `false`. The deployed client built successfully
with `[100%] Built target moonlight` (exit 0).

The standalone synthetic probe built, but its run exited **77**, before any
image import: opening `/dev/dma_heap/default_cma_region` failed with
`Permission denied`. Its final result was exactly:

```text
SELFTEST: NV12 external SKIP (CMA dma-buf allocation or mapping unavailable)
```

This is a skipped synthetic pixel gate, not a pixel-correctness pass. The
correctness evidence below comes from the active real-stream path and visual
inspection; the synthetic permission limitation remains unresolved.

Same-session baseline (`CASES="default trivial"`, compositor off). In the
historical tables below, `avg_fps` is the old harness summary, which averages
cumulative readings; `avg_delta_fps` is the mean rendered-frame window rate.
The old harness incorrectly labeled the latter as host rate. Neither is an
independent source-rate measure.

| case | samples | `avg_c2` | `avg_fps` | `avg_delta_fps` | harness result |
|---|---|---|---|---|---|
| default | 105 | 27.12 ms | 36.8 | 36.7 | FAIL |
| trivial | 189 | 14.53 ms | 59.0 | 59.6 | PASS |

Both baseline logs contained the required `EGL: nv12 two-plane import` marker
and 1920x1080 frame/view samples. The baseline script exited 1 because default
still failed, as expected. Trivial passed its `avg_c2 < 16.7 ms` control gate,
with a near-60 rendered-frame window rate, so the environment could measure the
candidate. Default's delta was only 36.7 fps; it does not measure how many
frames the source supplied.

Two separate `CASES=external` sessions, each with breakdown timing enabled:

| run | samples | `avg_c2` | `avg_fps` | `avg_delta_fps` | script exit | harness result |
|---|---|---|---|---|---|---|
| external 1 | 182 | 16.51 ms | 57.9 | 59.0 | 1 | FAIL (`avg_fps < 58`) |
| external 2 | 166 | 15.77 ms | 57.9 | 55.8 | 1 | FAIL (`avg_fps < 58`) |

Both logs contained the exact marker `EGL: nv12 external path active`, with
`frame=1920x1080 pitch=1920 uv_off=2088960` (fd 27 in run 1, fd 28 in run 2),
and real 1920x1080 x11 samples. No fallback or import error was observed in the
extracted diagnostics. Both passed the strict `avg_c2 < 16.7 ms` requirement,
but neither passed the old harness's reported average frame rate. Run 1 later
reached an `avg_fps` sample of 58.1 with render delta 60 and `c2=16.6ms`,
illustrating why the cumulative rate and the mean of its readings must be
distinguished. These summaries are retained without being treated as valid
acceptance decisions.

The image cache remained stable after importing the three-buffer pool:

| run | breakdown window | image/import | render + finish | swap | creates | hits |
|---|---|---|---|---|---|---|
| external 1 | first | 0.55 ms | 13.34 ms | 2.39 ms | 3 | 27 |
| external 1 | late range | 0.49-0.54 ms | 12.49-12.98 ms | 3.09-3.65 ms | 3 | up to 1797 |
| external 2 | first | 0.60 ms | 13.26 ms | 3.27 ms | 3 | 27 |
| external 2 | late range | 0.34-0.43 ms | 12.50-12.75 ms | 0.66-0.84 ms | 3 | up to 1647 |

Run 2's rendered-frame delta fell to 28.8, 23.5 and 21.3 fps near the end, with
a final cumulative `avg_fps` sample of 52.5. This establishes a slowdown, but
does not establish source instability: source-rate evidence requires the
separate `submits` counts and elapsed time. No thresholds were relaxed and no
fallback measurements were counted as external results.

For the visual gate, a separate bounded external stream was captured around
eight seconds into the session. The X11 capture succeeded (exit 0), yielding a
1920x1080 RGB PNG at `/tmp/external.png`, copied to `F:/temp/external.png`. Its
log contained the active external marker with the same frame/pitch/offset and
no fallback; representative `c2` samples were 15.0-16.5 ms, with rendered-frame
delta initially around 60 fps. Original-resolution inspection showed correct
orientation, neutral dark/grey areas, clean saturated yellow lines and legible
white Chinese/Latin text and thin edges. No visible UV-swap tint, stride or
plane-offset corruption, tiling corruption or inversion was seen. This passes
the real-stream visual gate, but does not replace the skipped synthetic colour
and pixel checks or test every motion/synchronisation issue noted above.

Pre-fix matrix verification rebuilt the scratch client successfully (exit 0,
`[100%] Built target moonlight`) and ran `CASES="default trivial external"`:

| case | samples | `avg_c2` | `avg_fps` | `avg_delta_fps` | harness result |
|---|---|---|---|---|---|
| default | 116 | 26.93 ms | 36.6 | 36.3 | FAIL |
| trivial | 191 | 11.71 ms | 59.9 | 60.0 | PASS |
| external | 191 | 15.86 ms | 60.1 | 60.0 | PASS |

The baseline cases had the two-plane marker, and external had the genuine
active external marker. The aggregate script exit was 1 solely because default
failed. The external harness pass prompted one predefined, fresh two-run
confirmation block, with unchanged thresholds and duration. It was not by
itself treated as the required two-run acceptance result.

That preliminary confirmation exposed a reporting defect. Its first run ended
at cumulative `avg_fps=60.0`; the second ended at `avg_fps=57.8` (frame 1840,
`delta_fps=60.1`, `c2=14.6ms`), although the old harness reported an average of
59.8 and PASS. The old `qs/qn` calculation averaged cumulative frame-rate
readings and could hide a late slowdown. In `src/video/x11.c`, `avg_fps` is
rendered frames divided by elapsed time since the render path started, whereas
`delta_fps` is 10 rendered frames divided by the latest reporting interval.
Calling that delta the host rate was also incorrect. These old summaries were
not used to claim acceptance.

The reporting fix in `7996854` makes `scripts/h618-accept.sh` retain the last
cumulative value and report `final_avg_fps`, which is the value used by the
unchanged strict `>= 58` gate.
It still averages the ten-frame `c2` windows for the unchanged `< 16.7 ms` gate.
For each line containing both `submits` and `delta_fps`, it estimates incoming
decoder submissions as `submits * delta_fps / 10`, and reports the arithmetic
mean of those paired estimates as `avg_submit_fps (decoder submissions)`.
This is a window-rate estimate from rounded timing data, not a separately
measured host rate, and it cannot override the rendered-frame gate.

The observed 59.8/PASS versus final 57.8 result was the regression's RED
evidence. A reduced log fixture reproduced that false pass before the fix;
after the fix it returned `final_avg_fps=57.8 FAIL` with exit 1 and correctly
reported 60.0 decoder submissions per second from paired fields, ignoring a
delta-only diagnostic line. Independent review also caught AWK string comparison
after `sub()`: the new final-rate value is explicitly coerced with `+0` before
comparison. Numeric regression fixtures first reproduced a false pass at 9.0 fps
and a false failure at 100.0 fps, then passed after coercion. `sh -n` and
`git diff --check` passed. The coercion does not change the observed 59.9/57.9
live outcomes.
