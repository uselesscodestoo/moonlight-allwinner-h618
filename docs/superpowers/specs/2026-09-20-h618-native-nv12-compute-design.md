# H618 1080p60: native NV12 external sampling, then compute

Date: 2026-09-20
Status: approved design; implementation plan pending
Worktree / branch: `F:\temp\wt-60fps` / `h618-60fps`

## 1. Context

The target is real 1920x1080 decode and display at 60 fps. Reducing the decode
resolution is not acceptable. Cedrus already decodes a 1080p frame in about
3.5 ms; all remaining cost is after decode.

The current linear-NV12 zero-copy path imports the decoder dma-buf as separate
`R8` luma and `GR88` chroma EGLImages. Its fragment shader performs one external
fetch from each plane per output pixel. At 1080p this costs about 25--29 ms per
frame and limits the client to about 34--36 fps. The no-sampling control reaches
60 fps, while EGLImage creation and swap are negligible.

The following alternatives have already been closed:

* a normal FBO copy into GPU-native textures regressed to about 36 ms/frame;
* changing dma-buf cache attributes has no useful lever because Panfrost maps
  imported BOs write-back;
* DE33 direct NV12 scanout is blocked by missing DE33 VI scaler/RCQ support;
* G2D cannot be brought up on the current mainline boot: only its TOP wrapper
  responds, even after reproducing all publicly known reset and clock setup.

A vendor-firmware register trace remains useful but is deferred until separate
boot media is available.

## 2. Goal and non-goals

### Goal

Find a post-decode path that repeatedly displays real 1920x1080 HEVC at 60 fps:

* `avg_c2 < 16.7 ms`;
* `avg_fps >= 58` while the source rate is approximately 60 fps;
* correct BT.601/BT.709 and limited/full-range colour;
* no CPU readback and no decode-resolution reduction.

### Non-goals

* vendor-firmware boot or G2D register tracing;
* implementing the DE33 VI scaler/RCQ stack;
* Vulkan or OpenCL paths;
* replacing the proven current path before a candidate passes acceptance;
* fixing the separately recorded HEVC first-B-after-IDR discrepancy or tear.

## 3. Candidate order

The experiments are intentionally ordered by cost and reversibility.

1. **Native multi-plane NV12 external texture (preferred).** Import one
   `DRM_FORMAT_NV12` EGLImage with two planes and sample it through
   `samplerExternalOES`. Mesa/Panfrost owns chroma sampling and YUV-to-RGB
   conversion. This is one pass and one logical texture operation per pixel.
2. **GLES 3.1 compute conversion.** If native external sampling misses the
   target, dispatch one compute invocation per 2x2 output block. Four Y samples
   and one shared UV sample produce four RGBA pixels in a GPU-native texture,
   which is then presented by a trivial draw.
3. **V4L2 capture DMABUF import.** Only if both GPU experiments fail, test
   whether Cedrus accepts user-allocated contiguous buffers and whether buffer
   provenance changes Panfrost sampling cost. This is lower-confidence because
   the current capture buffers are already CMA/coherent and GPU imports are
   already cacheable.

## 4. Architecture and isolation

The current `R8 + GR88` renderer remains the default and unconditional fallback.
Experiments are opt-in:

* `MOONLIGHT_ZC_EXTERNAL=1` selects native NV12 external sampling;
* `MOONLIGHT_ZC_COMPUTE=1` selects compute conversion after that experiment is
  implemented;
* setting neither variable preserves current behaviour.

The two experiment variables are mutually exclusive. If both are set, the
client logs the configuration error and uses the current renderer rather than
silently choosing one.

`egl_draw_dmabuf_nv12()` keeps its public draw interface. Internally it tries
the requested experimental path and falls through to the current renderer on
capability, shader, import, or draw failure. Each path prints a one-time line so
acceptance logs prove which renderer actually ran.

The native external experiment changes `src/video/egl.c`, `src/video/egl.h`,
and `src/video/x11.c`. It adds a small explicit colour-metadata setter because the existing
`egl_set_color_params()` exposes only derived floating-point coefficients while
EGL import needs the original 601/709 and limited/full choices. Inferring those
choices back from coefficient values is deliberately avoided.

No experimental source is deployed over the board's main repository. Files are
copied only from this worktree to `~/Downloads/probe/moonlight-dev` for builds
and live runs.

## 5. Native NV12 external path

### 5.1 Capability gate

The path requires all of:

* EGL dma-buf import and modifier support already used by the project;
* `GL_OES_EGL_image_external`;
* `GL_OES_EGL_image_external_essl3`;
* `glEGLImageTargetTexture2DOES`.

Missing capability is a normal fallback, not a fatal initialization error.

### 5.2 EGLImage

Create a single `EGL_LINUX_DMA_BUF_EXT` image with:

* width and height equal to the decoded frame;
* `DRM_FORMAT_NV12`, linear modifier;
* plane 0: decoder fd, offset 0, luma pitch;
* plane 1: the same fd, decoder-provided UV offset and pitch;
* EGL YUV colour-space, sample-range, and chroma-siting hints.

The image cache key contains fd, dimensions, pitch, UV offset, colour matrix,
and range. Resolution or colour-metadata changes invalidate the cache. Failed
imports are negatively cached so a bad fd does not trigger two EGL calls per
frame.

### 5.3 Draw

Bind the image to `GL_TEXTURE_EXTERNAL_OES`. An ESSL 3 external-texture fragment
shader computes only the normalized source coordinate, scaling, and vertical
flip, then performs one `texture()` call. Mesa/Panfrost returns RGB after its
NV12 sampling and conversion. The existing surface-size query and fullscreen
triangle are retained.

The stream metadata maps to EGL hints as follows:

* BT.601 -> `EGL_ITU_REC601_EXT`;
* BT.709 -> `EGL_ITU_REC709_EXT`;
* limited range -> `EGL_YUV_NARROW_RANGE_EXT`;
* full range -> `EGL_YUV_FULL_RANGE_EXT`.

The implementation must use the chroma-siting values appropriate to the NV12
frames produced by this decoder. The synthetic probe and real screenshot check
gate the choice; it is not guessed from RGB appearance alone.

## 6. Compute fallback experiment

This section is implemented only if native external sampling fails correctness
or performance acceptance.

Initialization first verifies that the actual context reports OpenGL ES 3.1 or
newer and that the required compute/image-store limits cover the chosen local
size and RGBA8 output. A merely ES 3.0 context is a normal fallback.

Reuse the existing luma `R8` and chroma `GR88` EGLImages. A GLES 3.1 compute
shader assigns one invocation to one 2x2 luma block:

1. fetch four adjacent Y samples;
2. fetch the block's one shared UV sample;
3. apply the existing colour coefficients to four pixels;
4. write four RGBA8 pixels to a GPU-native output image.

This changes external fetch demand from eight logical per-pixel-plane accesses
per 2x2 block to five explicit accesses. Workgroups cover contiguous blocks so
texture requests and output stores are coalesced. Bounds checks handle odd
dimensions even though the acceptance size is even.

After dispatch, issue the shader-image and texture-fetch memory barriers, then
draw the result with a trivial fullscreen shader. The first experiment is
synchronous and single-output-texture: pipelining/double buffering is considered
only if the compute conversion is close to, but not below, 16.7 ms.

Use `GL_EXT_disjoint_timer_query`, which is present on the board, to separate
compute time from the final draw. Existing `MOONLIGHT_ZC_BREAKDOWN=1` remains the
end-to-end cross-check.

## 7. Correctness probe

Add a small standalone tool that allocates a contiguous dma-buf from
`/dev/dma_heap/default_cma_region`, fills a linear NV12 image with known colour
cases, and exercises the same EGLImage attributes and external shader as the
client. It renders offscreen and reads RGB back for comparison with a CPU
reference.

The cases cover black, white, neutral grey, and chromatic samples for limited
BT.601, plus at least one BT.709 and one full-range case. Maximum accepted RGB
error is two 8-bit code values per component. The tool must use dma-buf CPU
access synchronization around writes.

If the board cannot import this CMA allocation into Panfrost, the tool reports
that allocator limitation explicitly. Real decoder-frame validation still runs;
the tool failure must not be misreported as proof that decoder-exported buffers
are unsupported.

## 8. Performance acceptance

Run an A/B matrix under identical conditions: active VT, Mali renderer,
compositor off, never-blank/DPMS prepared, and a real 1080p60 HEVC stream.

1. current `R8 + GR88` baseline;
2. `MOONLIGHT_ZC_EXTERNAL=1`;
3. if needed, `MOONLIGHT_ZC_COMPUTE=1`;
4. trivial and CPU-download controls for regression evidence.

A candidate passes only when two consecutive runs show:

* the requested path selected with no fallback;
* `avg_c2 < 16.7 ms`;
* `avg_fps >= 58` and source rate approximately 60 fps;
* EGLImage creation count stable after the initial decoder surface pool;
* colour probe passed and a captured real frame has correct orientation and
  colour.

The current active Moonlight process is never terminated implicitly. Live tests
start only when the display is free or after explicit user authorization.

## 9. Failure handling and rollback

* Shader compile/link and EGL/GL errors name the experimental path and are rate
  limited.
* Unsupported extensions, invalid descriptors, and failed imports return to the
  current renderer for the frame.
* Experimental caches and GL objects are destroyed during the existing
  resolution-reset and `egl_destroy()` lifecycle.
* No candidate becomes the default merely because it builds or looks correct.
  It must pass the complete correctness and two-run performance gate.
* A failed candidate remains opt-in only if its code is useful diagnostically;
  otherwise it is reverted and only the measurements are documented.

## 10. Deferred branch: capture-buffer provenance

If both external and compute paths fail, the next specification begins with a
non-destructive capability probe for `V4L2_MEMORY_DMABUF` on the Cedrus capture
queue. Only after support is proven will the libva driver gain an optional path
that queues contiguous user-allocated buffers.

The first test compares the same EGL workload using a Cedrus-exported buffer and
a compatible user-allocated contiguous buffer. A full libva allocation rewrite
is not justified unless that A/B measurement materially reduces sampling time.

## 11. References

* Linux V4L2 dma-buf importing:
  https://docs.kernel.org/userspace-api/media/v4l/dmabuf.html
* Linux stateless decoder buffer model:
  https://docs.kernel.org/userspace-api/media/v4l/dev-stateless-decoder.html
* Mesa Panfrost supported APIs:
  https://docs.mesa3d.org/drivers/panfrost.html
