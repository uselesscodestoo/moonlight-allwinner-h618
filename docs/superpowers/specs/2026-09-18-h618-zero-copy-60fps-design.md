# H618 zero-copy display path: design to reach 1080p60

Date: 2026-09-18
Status: **Concluded (2026-09-19).**  Plan A (two-plane float) is implemented
(`a1c6b84`, review fixes `30a42bf`, diagnostic `8e5c3d1`); Plan C (GPU copy into
native textures) failed and was reverted; the kernel DE33 VI-plane route is
blocked upstream (chroma upsampling needs the DE33 VI scaler, unmerged).  The
zero-copy path settles at **~34-36 fps at 1080p** (`c2` ~25-29 ms): the GPU
sampling of the external dma-buf costs ~22 ms and is intrinsic.  X/vsync/import/
present are not the limit, and neither is the dma-buf's cache attribute
(panfrost maps every BO write-back).  The `<16.7 ms` (60 fps) goal was **not
reached** and is not reachable on the current mainline/Armbian stack.  Full
record: `docs/h618-zero-copy.md`.
Worktree / branch: `F:\temp\wt-moonlight-drm` @ `h618-drm-kms`
Primary repo: `moonlight-embedded` (client). Fallback touches libva and the kernel.

## 1. Background and measured facts

The zero-copy path (`egl_draw_dmabuf_nv12()` in `src/video/egl.c`) imports the
decoder's linear-NV12 capture dma-buf as a single external `R8` image and does
three `texelFetch` per pixel with `highp` precision. Measurements on the
reference board (`orangepi`, H616, Mali-G31 MC1, Xorg + glamor, compositor
OFF), one real Sunshine session per configuration, `c2` = draw+swap:

| Run | Path | c2 | source rate |
|---|---|---|---|
| `default` | NV12 shader, vsync on | **29.0 ms** | 34.5 fps |
| `MOONLIGHT_ZC_TRIVIAL=1` | same import + present, no shader math | **16.6 ms** | **60.0 fps** |
| `MOONLIGHT_NO_VSYNC=1` | NV12 shader, vsync off | 34.7 ms | 28.7 fps |
| `MOONLIGHT_NO_ZC=1` | CPU download (`c1`=43.6, `c2`=3.1, `c3`=5.0) | 3.1 ms | 19.3 fps |

Synthetic `egl_bench` (software-upload path, same X session):

| Config | vsync | ms/frame |
|---|---|---|
| compositor OFF, 1080p | on / off | 16.75 / 16.71 |
| compositor OFF, 720p | on / off | 16.83 / 16.40 |
| compositor ON, 1080p | on / off | 26.29 / 26.60 |

Conclusions:

1. Import + present + X (compositor off) sustains **60 fps** (trivial run).
2. The CPU-download path's draw+swap is **3.1 ms**; its 3 `texture()` fetches on
   GPU-native textures are cheap.
3. The ~13 ms deficit (29.0 - 16.6) is entirely the **NV12 fragment shader** on
   the imported external `R8` texture.
4. It is not vsync waiting: disabling vsync makes it slower (GPU-saturated).
5. Not the kernel and not X: bypassing X (bare DRM) would run the same shader.

## 2. Root cause hypothesis

The fast software shader (`egl.c:57-80`) is `mediump`, samples via `texture()`
with an interpolated varying, on GPU-native textures. The slow NV12 shader
(`egl.c:149-192`) is `highp`, samples with `texelFetch` at integer addresses
computed per pixel (`2*cx`, `uv_row + cy`) into one large (`1920x1632`) external
linear `R8` image.

Two candidate mechanisms, to be separated by the Stage 0 experiments:

* **H1 - external/linear texture path is slow** (uncached/uncached-write-combine
  dma-buf, poor texture-cache behaviour).
* **H2 - `texelFetch` + `highp` + dependent integer addressing** is slow
  relative to `texture()` + `mediump` + varyings.

Note the trivial run proves re-importing the EGLImage each frame is not the cost.

## 3. Goals / non-goals

### Goals

* The zero-copy path (`x11_vaapi`, HEVC, **1920x1080**) sustains 60 fps: per-frame
  `c2 < 16.7 ms` and rendered `avg_fps >= 58` while the host sends 60 fps
  (`delta_fps ≈ 60`; no dropped frames).
* The physical per-frame display cost is understood and documented.

### Non-goals

* 720p, SD, H264 at 60 fps (must stay correct, not accelerated).
* Fixing the vertical tear. Requirement is **no regression**; a control run is
  recorded.
* Any kernel work unless triggered (see 5.4).
* Changing moonlight's architecture beyond the display path.

## 4. Acceptance criteria

Hard (all must pass, 1080p HEVC, `-fps 60`, compositor off):

1. `ml_matrix.sh` `default` run: `avg_c2 < 16.7 ms`.
2. Same run: rendered `avg_fps >= 58` while `delta_fps ≈ 60` (host sending 60,
   nothing dropped); the `submits=` field (host decode units per window) tracks
   the rendered frame count.
3. `MOONLIGHT_COLOR_SELFTEST=1` prints `SELFTEST: PASSED` (byte/BT.601 correctness).
4. `MOONLIGHT_NO_ZC=1` and the tiled fallback (`MOONLIGHT_ZC_TILED=1`,
   720x480) still work and are not slower than recorded today.
5. `MOONLIGHT_ZC_TRIVIAL=1` remains ~1 vblank (sanity: import/present unchanged).

Soft (recorded, not gating):

* Tear control: same content with `MOONLIGHT_NO_ZC=1`; both sides recorded.

## 5. Approach

### 5.1 Plan A - fix the sampling path (primary, no extra copy)

Build **two** EGLImages from the same decoder dma-buf and sample with
`texture()` + `mediump`:

* luma: `EGL_LINUX_DRM_FOURCC_EXT = DRM_FORMAT_R8`, offset 0, pitch 1920,
  height = frame height; sample `.r`.
* chroma: `DRM_FORMAT_GR88`, offset = `desc->layers[0].offset[1]` (2088960),
  pitch 1920, height = frame_h/2; sample `.r` = U, `.g` = V.

This removes the giant texture, the dependent `texelFetch` addressing, and the
`highp` math, and halves the chroma fetches. Expected `c2` near the software
path's ~3 ms; the 16.6 ms present ceiling leaves ample headroom.

### 5.2 Plan B - compute shader (escalation)

GLES 3.1 compute reads NV12 and writes RGB into a render target, then a trivial
draw presents it. Extra pass, more code. Only if A cannot get under 16.7 ms.

### 5.3 Plan C - GPU blit into a GPU-native texture (contingency)

One GPU copy of the 3 MB buffer into a GPU-native texture, then the existing
shader. Robust; costs an extra pass. Not selected by the user; use only if A
and B fail and kernel work is rejected.

### 5.4 Kernel - DE33 VI plane YUV (fallback, own spec)

Restore YUV formats to the H616 VI layer so the display engine scans the
decoder's NV12 directly (hardware CSC, no GPU, clean KMS-flip sync). This is a
separate kernel project; it is in scope here only as a documented escalation.

### 5.5 Escalation triggers

* A's Stage-0 micro-experiment (section 8) fails to reach `c2 < 16.7 ms` after
  both H1/H2 isolation variants -> try B.
* B fails -> reconsider C or start 5.4 as its own spec.

## 6. Detailed design (Plan A)

Files:

* `src/video/egl.c`
  * Replace the single-image NV12 import with a per-fd cache holding two
    `EGLImageKHR`s (luma `R8`, chroma `GR88`). Cache key: dma-buf fd; invalidate
    on resolution change (as today).
  * New fragment shader: `#version 300 es`, `precision mediump`, two
    `sampler2D` inputs, sample `texture(u_y, uv)` and `texture(u_uv, uv)`,
    apply the existing `u_yscale/u_yoff/u_rv/u_gu/u_gv/u_bu` colour coefficients
    (must reproduce `upload_color_params()` semantics exactly).
  * Keep the `MOONLIGHT_ZC_TRIVIAL` branch and the viewport uniforms.
  * Exactness: prefer `texelFetch` only if `texture()` with NEAREST is not
    bit-exact; decide from the Stage-0 result and the colour self-test.
* `src/video/x11.c`: pass the chroma plane offset/pitch to the new entry point
  (already available in the descriptor); keep the dispatch on
  `drm_format_modifier == 0`.
* `src/video/ffmpeg_vaapi.[ch]`: only if the descriptor does not already expose
  what we need (it does: `layers[0].offset[1]`, `layers[0].pitch[0]`).

Fallbacks untouched: tiled (`SUNXI_TILED_NV12`) stays opt-in and slow; CPU
download stays the default fallback; all `MOONLIGHT_*` switches keep working.

Colour range/space: coefficients come from the stream metadata
(`egl_set_color_params()`); no change expected, but the self-test gates it.

## 7. Testing and verification

* Build/deploy: local worktree -> `rsync` changed sources to a board scratch
  tree (`~/Downloads/probe/moonlight-dev`, a copy of the user's repo; the user's
  repo is never edited) -> `cmake --build`.
* Acceptance run: `ml_matrix.sh` (runs `default`, `MOONLIGHT_ZC_TRIVIAL`,
  `MOONLIGHT_NO_VSYNC`, `MOONLIGHT_NO_ZC`, parses `c2`/`avg_fps`).
* Correctness: `MOONLIGHT_COLOR_SELFTEST=1` and the existing
  `handoff/verify.sh` decode checks must stay green.
* Tear recording: `MOONLIGHT_NO_ZC=1` control run, same content.
* Preconditions: `xset s off; xset +dpms; xset dpms 0 0 0; xset s noblank;
  xset dpms force on`; compositor off (`xfconf-query ... use_compositing false`,
  restored after); VT active on tty7.

## 8. Iteration harness (Stage 0 experiments)

Goal: separate H1 (external texture) from H2 (texelFetch/highp), with minimal
code, before committing to the full change. Each variant is a small edit to
`egl.c`, built on the board, measured with `ml_matrix.sh` `default`:

1. **V1 - precision/addressing only**: keep the single external `R8` image, but
   switch the shader to `mediump` and computed normalized `texture()`.
2. **V2 - two images**: luma `R8` + chroma `GR88`, `mediump`, `texture()` (the
   Plan A target).
3. **V3 - two images, `texelFetch`**: only if V2 regresses on exactness.

Decision: if V1 already reaches the target, ship the minimal change; if V1
helps but not enough and V2 reaches it, ship Plan A.

## 9. Risks and mitigations

* **Panfrost rejects the `GR88`/offset import** -> fall back to V1; if that
  fails, Plan B/C.
* **`texture()` breaks byte-exactness of the colour conversion** -> use
  `texelFetch` on the two-plane images (V3), or keep NEAREST + explicit
  rounding; gated by the self-test.
* **External-buffer sampling is inherently slow (H1)** and even two images do
  not help -> Plan B (compute) reads via SSBO/image, or Plan C blit.
* **Noise in measurements** (Sunshine variable rate) -> always compare against
  the `ZC_TRIVIAL` control in the same session; use `c2`, not end-to-end fps.

## 10. Open questions

* Does `texture()` at NEAREST on these images stay bit-exact with the current
  `texelFetch` output? (resolved by Stage 0 V2/V3 + self-test)
* Is `DRM_FORMAT_GR88` sampling on panfrost (`U` in `.r`, `V` in `.g`) confirmed?
  (resolved by the self-test)

## 11. Change list (expected)

* `src/video/egl.c` (main)
* `src/video/x11.c` (call the new entry point; no logic change)
* `docs/h618-zero-copy.md` (record the result)
* `tools/`/`scripts/` if a small acceptance helper is added
* this spec
