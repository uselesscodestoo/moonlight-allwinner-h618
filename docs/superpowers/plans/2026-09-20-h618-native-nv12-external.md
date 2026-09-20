# H618 Native NV12 External Sampling Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Determine whether Panfrost's native multi-plane NV12 external sampler can display real 1920x1080 HEVC at 60 fps without CPU readback.

**Architecture:** Keep the current `R8 + GR88` renderer as the default and fallback. Behind `MOONLIGHT_ZC_EXTERNAL=1`, import the decoder fd once as a two-plane `DRM_FORMAT_NV12` EGLImage, bind it to `GL_TEXTURE_EXTERNAL_OES`, and render with one `samplerExternalOES` lookup per output pixel. Pass discrete 601/709 and limited/full metadata from `x11.c`, validate colour through the same production renderer using a CMA dma-buf probe, and accept the path only after two real-stream runs meet the 60 fps gate.

**Tech Stack:** C, EGL 1.5 dma-buf import, OpenGL ES 3 / `GL_OES_EGL_image_external_essl3`, X11, Linux dma-heap/dma-buf, FFmpeg colour metadata, CMake/Ninja.

**Spec:** `docs/superpowers/specs/2026-09-20-h618-native-nv12-compute-design.md`

---

## Scope and file map

This plan covers only the first candidate in the spec. A failed performance
result ends this plan and triggers a separate compute-shader plan.

* `scripts/h618-accept.sh` — selectable real-stream cases and an external-path
  marker gate; this is the failing acceptance test written first.
* `src/video/egl.h` — discrete colour-mode setter and external self-test API.
* `src/video/x11.c` — passes the stream's original matrix/range choice.
* `src/video/egl.c` — external shader, capability gate, two-plane EGLImage
  cache, fallback draw, instrumentation, lifecycle, and production self-test.
* `tools/egl_nv12_external_probe.c` — small X11 wrapper that runs the production
  self-test independently of a Sunshine stream.
* `docs/h618-zero-copy.md` — measured result and decision.

The source of truth is `F:\temp\wt-60fps`. Deploy only to
`scy@172.30.71.3:~/Downloads/probe/moonlight-dev`; never edit the board's
`~/Downloads/moonlight-embedded` repository.

---

### Task 1: Add the failing external-path acceptance case

**Files:**
- Modify: `scripts/h618-accept.sh`

- [ ] **Step 1: Make `report` require an optional path marker**

Change the function header and add the marker check before the `awk` parser:

```sh
report() {
  name="$1"; log="$2"; strict="$3"; marker="$4"
  echo "----- $name -----"
  grep -aE "nv12 .*import|nv12 external|EGL: vsync|renderer=" "$log" | head -3
  if [ -n "$marker" ] && ! grep -aqF "$marker" "$log"; then
    echo "  FAIL(path marker missing: $marker)"
    return 1
  fi
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
```

- [ ] **Step 2: Pass a marker through `run`**

Replace the first lines of `run` and its `report` call:

```sh
run() {
  name="$1"; strict="$2"; marker="$3"; shift 3
  # existing launch/wait code is unchanged
  if ! report "$name" "$log" "$strict" "$marker"; then
    failures=$((failures + 1))
  fi
}
```

- [ ] **Step 3: Add selectable cases**

Replace the two unconditional `run` lines with:

```sh
CASES="${CASES:-default trivial}"
for case_name in $CASES; do
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
```

- [ ] **Step 4: Verify the test fails for the right reason**

First ensure the screen is free:

```sh
ssh scy@172.30.71.3 'pgrep -a moonlight || echo none'
```

If a Moonlight process is listed, stop and ask the user; do not terminate it.
Otherwise deploy only the script and run the current binary:

```sh
scp scripts/h618-accept.sh scy@172.30.71.3:~/Downloads/probe/h618-accept.sh
ssh scy@172.30.71.3 'sed -i "s/\r$//" ~/Downloads/probe/h618-accept.sh; chmod +x ~/Downloads/probe/h618-accept.sh; CASES=external DUR=12 HOST=172.31.30.147 ML=$HOME/Downloads/probe/moonlight-dev/build/moonlight ~/Downloads/probe/h618-accept.sh; echo exit=$?'
```

Expected: `FAIL(path marker missing: EGL: nv12 external path active)` and
`exit=1`. A connection failure or `FAIL(no samples)` is not the required RED;
fix the environment and repeat.

- [ ] **Step 5: Commit the failing acceptance case**

```sh
git add scripts/h618-accept.sh
git commit -m "test: add native NV12 external acceptance case"
```

---

### Task 2: Preserve discrete stream colour metadata

**Files:**
- Modify: `src/video/egl.h`
- Modify: `src/video/egl.c`
- Modify: `src/video/x11.c`

- [ ] **Step 1: Add the colour-mode API**

In `src/video/egl.h`, after `egl_set_color_params`, add:

```c
/* Preserve the discrete metadata needed by EGL's native YUV conversion. */
void egl_set_color_mode(int use_bt709, int full_range);

/* Runs the real external-texture renderer on a synthetic CMA NV12 dma-buf.
 * Returns 0 on pass, 77 when the allocator/import cannot be tested, 1 on fail. */
int egl_nv12_external_selftest(void);
```

- [ ] **Step 2: Store the mode without reverse-engineering coefficients**

Near the existing colour coefficient state in `egl.c`, add:

```c
static int color_use_bt709;
static int color_full_range;

void egl_set_color_mode(int use_bt709, int full_range) {
  color_use_bt709 = !!use_bt709;
  color_full_range = !!full_range;
}
```

The default zero values intentionally match the current limited-range BT.601
default.

- [ ] **Step 3: Pass the original decision from `x11.c`**

In `apply_stream_colors()`, immediately before `egl_set_color_params(...)`, add:

```c
  egl_set_color_mode(use709, full);
```

- [ ] **Step 4: Build the metadata-only change**

Deploy the three files into the scratch tree and build:

```sh
scp src/video/egl.c src/video/egl.h src/video/x11.c scy@172.30.71.3:~/Downloads/probe/moonlight-dev/src/video/
ssh scy@172.30.71.3 'sed -i "s/\r$//" ~/Downloads/probe/moonlight-dev/src/video/{egl.c,egl.h,x11.c}; cd ~/Downloads/probe/moonlight-dev && cmake --build build -j2'
```

Expected: build exit 0 with `Built target moonlight`.

- [ ] **Step 5: Commit the metadata plumbing**

```sh
git add src/video/egl.h src/video/egl.c src/video/x11.c
git commit -m "video/egl: preserve discrete stream colour metadata"
```

---

### Task 3: Add the native NV12 EGLImage and external shader

**Files:**
- Modify: `src/video/egl.c`

- [ ] **Step 1: Add missing format/target fallbacks and extension detection**

Next to the existing DRM format fallbacks, add:

```c
#ifndef DRM_FORMAT_NV12
#define DRM_FORMAT_NV12 0x3231564e /* 'N', 'V', '1', '2' */
#endif
#ifndef GL_TEXTURE_EXTERNAL_OES
#define GL_TEXTURE_EXTERNAL_OES 0x8D65
#endif
#ifndef EGL_YUV_COLOR_SPACE_HINT_EXT
#define EGL_YUV_COLOR_SPACE_HINT_EXT 0x327B
#define EGL_SAMPLE_RANGE_HINT_EXT 0x327C
#define EGL_YUV_CHROMA_HORIZONTAL_SITING_HINT_EXT 0x327D
#define EGL_YUV_CHROMA_VERTICAL_SITING_HINT_EXT 0x327E
#define EGL_ITU_REC601_EXT 0x327F
#define EGL_ITU_REC709_EXT 0x3280
#define EGL_YUV_FULL_RANGE_EXT 0x3282
#define EGL_YUV_NARROW_RANGE_EXT 0x3283
#define EGL_YUV_CHROMA_SITING_0_EXT 0x3284
#endif
#ifndef EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT
#define EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT 0x3445
#define EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT 0x3446
#endif

static int has_gl_extension(const char* wanted) {
  GLint count = 0;
  glGetIntegerv(GL_NUM_EXTENSIONS, &count);
  for (GLint i = 0; i < count; i++) {
    const char* ext = (const char*)glGetStringi(GL_EXTENSIONS, i);
    if (ext && strcmp(ext, wanted) == 0)
      return 1;
  }
  return 0;
}
```

- [ ] **Step 2: Add the external shader**

Place this after `dmabuf_vertex_source`:

```c
static const char* nv12_external_fragment_source = "\
#version 300 es\n\
#extension GL_OES_EGL_image_external_essl3 : require\n\
precision mediump float;\n\
uniform samplerExternalOES u_tex;\n\
uniform vec2 u_view_size;\n\
layout(location = 0) out vec4 outColor;\n\
void main() {\n\
  vec2 uv = vec2(gl_FragCoord.x / u_view_size.x,\n\
                 1.0 - gl_FragCoord.y / u_view_size.y);\n\
  outColor = texture(u_tex, uv);\n\
}\n";
```

- [ ] **Step 3: Add state and a complete cache key**

Near the current NV12 state, add:

```c
static int nv12_external_requested;
static int nv12_external_available;
static GLuint nv12_external_program;
static GLuint nv12_external_texture;
static GLint nv12_external_tex_uniform;
static GLint nv12_external_view_uniform;
static struct {
  int fd, width, height, pitch, uv_offset;
  int use_bt709, full_range;
  EGLImageKHR image;
} dmabuf_nv12_external[DMABUF_MAX_IMAGES];
static int dmabuf_nv12_external_count;
```

- [ ] **Step 4: Create and cache a native NV12 image**

Add a helper beside `dmabuf_get_nv12()`:

```c
static EGLImageKHR dmabuf_get_nv12_external(int fd, int frame_width,
                                             int frame_height, int pitch,
                                             int uv_offset) {
  for (int i = 0; i < dmabuf_nv12_external_count; i++) {
    if (dmabuf_nv12_external[i].fd == fd &&
        dmabuf_nv12_external[i].width == frame_width &&
        dmabuf_nv12_external[i].height == frame_height &&
        dmabuf_nv12_external[i].pitch == pitch &&
        dmabuf_nv12_external[i].uv_offset == uv_offset &&
        dmabuf_nv12_external[i].use_bt709 == color_use_bt709 &&
        dmabuf_nv12_external[i].full_range == color_full_range) {
      zc_img_hits++;
      return dmabuf_nv12_external[i].image;
    }
  }
  if (dmabuf_nv12_external_count >= DMABUF_MAX_IMAGES)
    return EGL_NO_IMAGE_KHR;

  EGLint attrs[] = {
    EGL_WIDTH, frame_width,
    EGL_HEIGHT, frame_height,
    EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_NV12,
    EGL_DMA_BUF_PLANE0_FD_EXT, fd,
    EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
    EGL_DMA_BUF_PLANE0_PITCH_EXT, pitch,
    EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, 0,
    EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, 0,
    EGL_DMA_BUF_PLANE1_FD_EXT, fd,
    EGL_DMA_BUF_PLANE1_OFFSET_EXT, uv_offset,
    EGL_DMA_BUF_PLANE1_PITCH_EXT, pitch,
    EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT, 0,
    EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT, 0,
    EGL_YUV_COLOR_SPACE_HINT_EXT,
      color_use_bt709 ? EGL_ITU_REC709_EXT : EGL_ITU_REC601_EXT,
    EGL_SAMPLE_RANGE_HINT_EXT,
      color_full_range ? EGL_YUV_FULL_RANGE_EXT : EGL_YUV_NARROW_RANGE_EXT,
    EGL_YUV_CHROMA_HORIZONTAL_SITING_HINT_EXT, EGL_YUV_CHROMA_SITING_0_EXT,
    EGL_YUV_CHROMA_VERTICAL_SITING_HINT_EXT, EGL_YUV_CHROMA_SITING_0_EXT,
    EGL_NONE
  };
  EGLImageKHR image = p_eglCreateImageKHR(display, EGL_NO_CONTEXT,
                                           EGL_LINUX_DMA_BUF_EXT, NULL, attrs);
  int slot = dmabuf_nv12_external_count++;
  dmabuf_nv12_external[slot].fd = fd;
  dmabuf_nv12_external[slot].width = frame_width;
  dmabuf_nv12_external[slot].height = frame_height;
  dmabuf_nv12_external[slot].pitch = pitch;
  dmabuf_nv12_external[slot].uv_offset = uv_offset;
  dmabuf_nv12_external[slot].use_bt709 = color_use_bt709;
  dmabuf_nv12_external[slot].full_range = color_full_range;
  dmabuf_nv12_external[slot].image = image; /* EGL_NO_IMAGE is a negative cache. */
  zc_img_creates++;
  return image;
}
```

These fallback values were checked against the board's
`/usr/include/EGL/eglext.h`; do not delete attributes merely to make
compilation pass.

- [ ] **Step 5: Add reset and destruction**

```c
static void dmabuf_nv12_external_reset(void) {
  for (int i = 0; i < dmabuf_nv12_external_count; i++)
    if (dmabuf_nv12_external[i].image != EGL_NO_IMAGE_KHR &&
        p_eglDestroyImageKHR)
      p_eglDestroyImageKHR(display, dmabuf_nv12_external[i].image);
  dmabuf_nv12_external_count = 0;
}
```

Call it beside `dmabuf_nv12_reset()` on resolution change and from
`egl_destroy()`. Delete `nv12_external_texture` and
`nv12_external_program` in `egl_destroy()` when non-zero.

- [ ] **Step 6: Compile the optional program in `egl_init()`**

After the context is current and extension entry points are loaded:

```c
  nv12_external_requested = getenv("MOONLIGHT_ZC_EXTERNAL") != NULL;
  if (nv12_external_requested && getenv("MOONLIGHT_ZC_COMPUTE")) {
    fprintf(stderr, "EGL: ZC external and compute are mutually exclusive; using current path\n");
    nv12_external_requested = 0;
  }
  nv12_external_available = nv12_external_requested &&
      has_gl_extension("GL_OES_EGL_image_external") &&
      has_gl_extension("GL_OES_EGL_image_external_essl3") &&
      p_eglCreateImageKHR && p_glEGLImageTargetTexture2DOES;
  if (nv12_external_available) {
    nv12_external_program = compile_program(dmabuf_vertex_source,
        nv12_external_fragment_source, "nv12-external");
    nv12_external_available = nv12_external_program != 0;
    if (nv12_external_available) {
      nv12_external_tex_uniform = glGetUniformLocation(nv12_external_program, "u_tex");
      nv12_external_view_uniform = glGetUniformLocation(nv12_external_program, "u_view_size");
      glGenTextures(1, &nv12_external_texture);
    }
  }
  if (nv12_external_requested && !nv12_external_available)
    fprintf(stderr, "EGL: native NV12 external path unavailable; using current path\n");
```

- [ ] **Step 7: Add the external draw helper**

```c
static int draw_dmabuf_nv12_external(int fd, unsigned int size,
                                     int frame_width, int frame_height,
                                     int pitch, int uv_offset,
                                     int surf_w, int surf_h,
                                     double* import_done) {
  uint64_t required = (uint64_t)uv_offset +
                      (uint64_t)pitch * ((frame_height + 1) / 2);
  if (!nv12_external_available || pitch < frame_width || uv_offset < 0 ||
      required > size)
    return -1;
  EGLImageKHR image = dmabuf_get_nv12_external(fd, frame_width, frame_height,
                                                pitch, uv_offset);
  if (image == EGL_NO_IMAGE_KHR)
    return -1;
  if (import_done)
    *import_done = zc_now_ms();

  while (glGetError() != GL_NO_ERROR) {}
  glUseProgram(nv12_external_program);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_EXTERNAL_OES, nv12_external_texture);
  p_glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, image);
  glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glUniform1i(nv12_external_tex_uniform, 0);
  glUniform2f(nv12_external_view_uniform, (float)surf_w, (float)surf_h);
  glViewport(0, 0, surf_w, surf_h);
  glDisable(GL_BLEND);
  glDisableVertexAttribArray(0);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
  return glGetError() == GL_NO_ERROR ? 0 : -1;
}
```

- [ ] **Step 8: Select it without breaking the existing path**

In `egl_draw_dmabuf_nv12()`, after making the context current, handling
resolution changes, and querying `surf_w/surf_h`, but before creating the two
single-plane images:

First change the entry validation so the external program can operate even if
the legacy two-plane shader is unavailable:

```c
  if ((!nv12_program && !nv12_external_available) || pitch <= 0 ||
      pitch < frame_width || uv_offset % pitch != 0)
    return -1;
```

After an external draw failure, return `-1` if `nv12_program` is zero;
otherwise continue into the unchanged two-plane fallback.

```c
  double bd1 = zc_breakdown ? zc_now_ms() : 0.0;
  if (nv12_external_requested &&
      draw_dmabuf_nv12_external(dmabuf_fd, size, frame_width, frame_height,
                                pitch, uv_offset, surf_w, surf_h,
                                zc_breakdown ? &bd1 : NULL) == 0) {
    static int active_shown;
    if (!active_shown++)
      fprintf(stderr, "EGL: nv12 external path active fd=%d frame=%dx%d pitch=%d uv_off=%d\n",
              dmabuf_fd, frame_width, frame_height, pitch, uv_offset);
    if (zc_breakdown) glFinish();
    double bd2 = zc_breakdown ? zc_now_ms() : 0.0;
    eglSwapBuffers(display, surface);
    double bd3 = zc_breakdown ? zc_now_ms() : 0.0;
    zc_record_breakdown(bd0, bd1, bd2, bd3);
    return 0;
  } else if (nv12_external_requested) {
    static int fallback_shown;
    if (fallback_shown++ < 5)
      fprintf(stderr, "EGL: nv12 external draw failed; falling back to two-plane path\n");
  }
```

Extract the existing accumulation/30-frame print block into
`zc_record_breakdown(double bd0, double bd1, double bd2, double bd3)` and call it
from both external and current paths. It must return immediately when
`zc_breakdown` is false. Move the current path's `dmabuf_get_nv12()` call after
the external attempt so external success does not create unused single-plane
images. In the current path, overwrite `bd1` immediately after
`dmabuf_get_nv12()`; in the external path, the helper writes it immediately
after the cached/imported NV12 EGLImage is obtained. This keeps `img`,
`render+finish`, and `swap` comparable between paths.

- [ ] **Step 9: Verify formatting and build**

```sh
git diff --check
scp src/video/egl.c scy@172.30.71.3:~/Downloads/probe/moonlight-dev/src/video/egl.c
ssh scy@172.30.71.3 'sed -i "s/\r$//" ~/Downloads/probe/moonlight-dev/src/video/egl.c; cd ~/Downloads/probe/moonlight-dev && cmake --build build -j2'
```

Expected: exit 0 and `Built target moonlight`. Do not start a stream yet.

---

### Task 4: Add a production-path CMA colour self-test

**Files:**
- Modify: `src/video/egl.c`
- Create: `tools/egl_nv12_external_probe.c`

- [ ] **Step 1: Add Linux-only dma-heap includes**

In `egl.c`:

```c
#ifdef __linux__
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#endif
```

- [ ] **Step 2: Add a synchronized CMA allocator**

Under `#ifdef __linux__`, add:

```c
static int alloc_cma_dmabuf(size_t size, void** map_out) {
  int heap = open("/dev/dma_heap/default_cma_region", O_RDWR | O_CLOEXEC);
  struct dma_heap_allocation_data alloc = {0};
  if (heap < 0)
    return -1;
  alloc.len = size;
  alloc.fd_flags = O_RDWR | O_CLOEXEC;
  if (ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &alloc) < 0) {
    close(heap);
    return -1;
  }
  close(heap);
  *map_out = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED,
                  (int)alloc.fd, 0);
  if (*map_out == MAP_FAILED) {
    close((int)alloc.fd);
    return -1;
  }
  return (int)alloc.fd;
}

static int dmabuf_sync(int fd, uint64_t flags) {
  struct dma_buf_sync sync = { .flags = flags };
  return ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
}
```

- [ ] **Step 3: Implement `egl_nv12_external_selftest()`**

Use a 64x64 image, pitch 64, UV offset 4096, total size 6144. Allocate the CMA
buffer, ensure the production context is current using the same `if (!current)
{ eglMakeCurrent(...); current = true; }` sequence as the draw functions, then
create a 64x64 RGBA8 renderbuffer/FBO. If the external path is unavailable,
print `SELFTEST: NV12 external SKIP (extension or shader unavailable)` and
return 77. For every test case:

```c
static const struct external_case {
  const char* name;
  int use709, full;
  uint8_t y, u, v;
  uint8_t expect_r, expect_g, expect_b;
} external_cases[] = {
  { "601 narrow black", 0, 0,  16, 128, 128,   0,   0,   0 },
  { "601 narrow white", 0, 0, 235, 128, 128, 255, 255, 255 },
  { "601 narrow grey",  0, 0, 126, 128, 128, 128, 128, 128 },
  { "601 narrow red",   0, 0,  81,  90, 240, 255,   0,   0 },
  { "709 narrow red",   1, 0,  63, 102, 240, 255,   1,   0 },
  { "601 full grey",    0, 1, 128, 128, 128, 128, 128, 128 },
};
```

For each case, perform `DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE`, fill all luma
bytes with `y` and every UV pair with `u,v`, then perform
`DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE`. Set the case colour mode, reset the
external cache so EGL receives new hints, create/import the image, bind the
test FBO, call `draw_dmabuf_nv12_external(...)` with a 64x64 view, and
`glReadPixels()`.

Check every pixel against the expected RGB with an absolute tolerance of 3.
Then add this exact 64x64 BT.601-narrow quadrant case (all boundaries are
2x2-aligned): source top-left `(Y,U,V)=(81,90,240)` red; source top-right
`(145,54,34)` green; source bottom-left `(41,240,110)` blue; source
bottom-right `(126,128,128)` grey. Because `glReadPixels` row zero is the
framebuffer bottom, require `(16,48)` red, `(48,48)` green, `(16,16)` blue,
and `(48,16)` grey in readback coordinates. Also require the four pairs
`(31,16)/(32,16)`, `(31,48)/(32,48)`, `(16,31)/(16,32)`, and
`(48,31)/(48,32)` to match their adjacent quadrant colours within tolerance.
This catches vertical inversion, UV byte swapping, plane-offset mistakes, and
a grossly wrong chroma-siting interpretation while the constant cases isolate
matrix and range selection.
Print one line per case and finish with exactly one of:

```text
SELFTEST: NV12 external PASSED
SELFTEST: NV12 external FAILED
SELFTEST: NV12 external SKIP (<specific allocator/import reason>)
```

Restore the previous framebuffer, viewport, colour mode, and external cache;
unmap and close the dma-buf on every exit. Return 0, 1, or 77 respectively.
On non-Linux builds, compile a function that prints `SKIP (non-Linux)` and
returns 77.

- [ ] **Step 4: Add the standalone wrapper**

Create `tools/egl_nv12_external_probe.c` following the window setup in
`tools/egl_bench.c`, but use a 64x64 window and this main body:

```c
int main(void) {
  Display* dpy = XOpenDisplay(NULL);
  if (!dpy) {
    fprintf(stderr, "cannot open display\n");
    return 1;
  }
  Window win = XCreateSimpleWindow(dpy, DefaultRootWindow(dpy),
                                    0, 0, 64, 64, 0, 0, 0);
  XMapWindow(dpy, win);
  XFlush(dpy);
  setenv("MOONLIGHT_ZC_EXTERNAL", "1", 1);
  egl_init(dpy, (NativeWindowType)win, 64, 64);
  int rc = egl_nv12_external_selftest();
  egl_destroy();
  XDestroyWindow(dpy, win);
  XCloseDisplay(dpy);
  return rc;
}
```

Include `stdio.h`, `stdlib.h`, `X11/Xlib.h`, and `video/egl.h`.

- [ ] **Step 5: Build and run the probe**

```sh
scp src/video/egl.c src/video/egl.h scy@172.30.71.3:~/Downloads/probe/moonlight-dev/src/video/
scp tools/egl_nv12_external_probe.c scy@172.30.71.3:~/Downloads/probe/moonlight-dev/tools/
ssh scy@172.30.71.3 'sed -i "s/\r$//" ~/Downloads/probe/moonlight-dev/src/video/{egl.c,egl.h} ~/Downloads/probe/moonlight-dev/tools/egl_nv12_external_probe.c; cd ~/Downloads/probe/moonlight-dev && gcc -O2 -o build/egl_nv12_external_probe tools/egl_nv12_external_probe.c src/video/egl.c $(pkg-config --cflags --libs egl glesv2 x11) -I src -I third_party/moonlight-common-c/src && DISPLAY=:0 ./build/egl_nv12_external_probe; echo exit=$?'
```

Expected: all cases `OK`, final `SELFTEST: NV12 external PASSED`, `exit=0`.
Exit 77 is an explicit allocator limitation; continue to the real decoder test
but record the missing synthetic gate. Exit 1 must be fixed before live testing.

- [ ] **Step 6: Commit the correct external renderer and probe**

```sh
git add src/video/egl.c src/video/egl.h src/video/x11.c tools/egl_nv12_external_probe.c
git commit -m "video/egl: sample native NV12 through an external texture"
```

---

### Task 5: Real-stream correctness and two-run performance gate

**Files:**
- Modify: `docs/h618-zero-copy.md`

- [ ] **Step 1: Verify the live-test preconditions**

```sh
ssh scy@172.30.71.3 'pgrep -a moonlight || echo none; DISPLAY=:0 eglinfo -B 2>/dev/null | grep "OpenGL ES profile renderer" | head -1; DISPLAY=:0 xfconf-query -c xfwm4 -p /general/use_compositing 2>/dev/null'
```

Required: no Moonlight process, renderer contains `Mali-G31 MC1 (Panfrost)`.
The harness may disable an active compositor and will restore its prior state.
If a process exists, stop and ask the user rather than invoking `quit` or
`pkill` yourself.

- [ ] **Step 2: Deploy the complete candidate**

```sh
scp src/video/egl.c src/video/egl.h src/video/x11.c scy@172.30.71.3:~/Downloads/probe/moonlight-dev/src/video/
scp scripts/h618-accept.sh scy@172.30.71.3:~/Downloads/probe/h618-accept.sh
ssh scy@172.30.71.3 'sed -i "s/\r$//" ~/Downloads/probe/moonlight-dev/src/video/{egl.c,egl.h,x11.c} ~/Downloads/probe/h618-accept.sh; chmod +x ~/Downloads/probe/h618-accept.sh; cd ~/Downloads/probe/moonlight-dev && cmake --build build -j2'
```

Expected: `Built target moonlight`.

- [ ] **Step 3: Capture the same-session baseline**

```sh
ssh scy@172.30.71.3 'CASES="default trivial" HOST=172.31.30.147 ML=$HOME/Downloads/probe/moonlight-dev/build/moonlight $HOME/Downloads/probe/h618-accept.sh; echo exit=$?'
```

Record default/trivial `avg_c2`, `avg_fps`, and source rate. The expected
historical shape is default failure around 25--29 ms and trivial pass near one
vblank. If trivial fails, stop: the environment cannot measure the candidate.

- [ ] **Step 4: Run the external candidate twice**

```sh
ssh scy@172.30.71.3 'for n in 1 2; do echo "=== external run $n ==="; CASES=external HOST=172.31.30.147 ML=$HOME/Downloads/probe/moonlight-dev/build/moonlight $HOME/Downloads/probe/h618-accept.sh || exit $?; done'
```

Both runs must show the active marker, `avg_c2 < 16.7 ms`, `avg_fps >= 58`,
source rate approximately 60, and exit 0. `zc breakdown` must show stable image
creation after the decoder surface pool is imported.

- [ ] **Step 5: Capture and inspect a real frame**

Run one external session, capture X11 while it is active, then explicitly end
only the session started by this command:

```sh
ssh scy@172.30.71.3 'ML=$HOME/Downloads/probe/moonlight-dev/build/moonlight; cleanup() { timeout 10 "$ML" quit 172.31.30.147 >/dev/null 2>&1 || true; }; trap cleanup EXIT; DISPLAY=:0 LIBVA_DRIVER_NAME=v4l2_request LIBVA_DRIVERS_PATH=$HOME/Downloads/v4l2-dri MOONLIGHT_ZC_EXTERNAL=1 timeout 20 "$ML" -platform x11_vaapi -codec h265 -1080 -fps 60 -localaudio -app Desktop stream 172.31.30.147 >/tmp/external_visual.log 2>&1 & p=$!; sleep 8; DISPLAY=:0 ffmpeg -y -loglevel error -f x11grab -video_size 1920x1080 -i :0 -frames:v 1 /tmp/external.png; wait "$p" || true'
scp scy@172.30.71.3:/tmp/external.png F:/temp/external.png
```

Inspect `F:\temp\external.png`: orientation, neutral greys, saturated colours,
text edges, and absence of plane-offset corruption must all be correct. Also
check `/tmp/external_visual.log` contains the active marker and no fallback.

- [ ] **Step 6: Record a deterministic outcome**

Append a dated subsection to `docs/h618-zero-copy.md` containing:

* synthetic probe result or explicit allocator skip;
* default/trivial baseline numbers;
* both external-run numbers and breakdown;
* screenshot result;
* one conclusion:
  * **accepted** only if every correctness gate and both performance runs pass;
  * **correct but too slow** if rendering is correct but either performance run
    misses the threshold; keep the path opt-in as diagnostic evidence and start
    a compute plan;
  * **rejected** if import or colour is wrong; revert the implementation commit,
    keep this results subsection, and start a compute plan.

- [ ] **Step 7: Run final verification**

```sh
git diff --check
git status --short
ssh scy@172.30.71.3 'cd ~/Downloads/probe/moonlight-dev && cmake --build build -j2'
```

For an accepted or correct-but-slow path, also rerun:

```sh
ssh scy@172.30.71.3 'CASES="default trivial external" HOST=172.31.30.147 ML=$HOME/Downloads/probe/moonlight-dev/build/moonlight $HOME/Downloads/probe/h618-accept.sh; echo exit=$?'
```

Report the actual exit status and all failing cases; do not call the experiment
successful based on build or visual correctness alone.

- [ ] **Step 8: Commit the measured result**

```sh
git add docs/h618-zero-copy.md
git commit -m "docs: record native NV12 external sampling results"
```

If the path was rejected and its implementation commit was reverted, include
the revert SHA and reason in the documentation commit. If it was correct but
too slow, the next action is a new compute-shader implementation plan; do not
start compute changes inside this plan.
