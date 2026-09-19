# H618 zero-copy Plan C: GPU blit off the present critical path

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Reach 1080p60 on the zero-copy path by moving the decoder-dma-buf sampling out of the display draw: copy it into GPU-native textures first, then present from those.

**Architecture:** Keep the two external EGLImages (luma `R8`, chroma `GR88`) as copy sources. Add a `GL_R8`/`GL_RG8` GPU texture pair and a trivial copy shader; each frame the copy pass renders the external planes into the GPU textures (framebuffer-attached), then the existing two-plane display shader samples the GPU textures and swaps. The hope is that the external read (~5 ms/plane) overlaps the vblank wait instead of serialising with the present, dropping `c2` under 16.7 ms.

**Tech Stack:** C, EGL/GLES 3, Panfrost/Mali-G31, CMake/Ninja.

**Supersedes:** Plan A (two-plane sampling) in `2026-09-18-h618-zero-copy-60fps.md`, which was implemented and committed (`a1c6b84`, `30a42bf`) but only reached `c2≈25 ms` (target `<16.7 ms`).

---

## Evidence behind Plan C (2026-09-19)

| Path | `c2` | fps |
|---|---|---|
| `trivial` (no fetch) | ~15 ms | 60 |
| luma-only (1 fetch) | 19.40 ms | 51.3 |
| chroma-only (1 fetch) | 19.43 ms | 51.4 |
| two-plane (2 fetches) | ~25 ms | 36 |

Each external plane fetch costs ~5 ms and the display path sits on the 16.7 ms cliff.
The external read must therefore be decoupled from the present, or the GPU sampling
must be eliminated (kernel DE plane). Plan C is the cheap attempt; if it fails,
escalate to a kernel/DE spec.

## Preconditions / context

- Worktree `F:\temp\wt-moonlight-drm`, branch `h618-drm-kms`, HEAD `30a42bf`.
- Board scratch tree `~/Downloads/probe/moonlight-dev` (ssh `scy@172.30.71.3`).
- Acceptance harness `~/Downloads/probe/h618-accept.sh` (`HOST`, `ML` env).
- Baseline to beat: `default avg_c2 < 16.7 ms`, `avg_fps >= 58`, SELFTEST PASSED.
- Never edit `/home/scy/Downloads/moonlight-embedded`.

---

## Task C1: Copy to GPU textures, then present

**Files:**
- Modify: `src/video/egl.c`

- [ ] **Step 1: Add the copy shader and state**

Add near `nv12_fragment_source`:

```c
/* Plan C: copy an imported plane into a GPU-native texture (one texelFetch,
 * no colour math). The display shader then samples GPU memory. */
static const char* nv12_copy_fragment_source = "\
#version 300 es\n\
precision highp float;\n\
precision highp int;\n\
precision highp sampler2D;\n\
uniform sampler2D u_src;\n\
layout(location = 0) out vec4 outColor;\n\
void main() {\n\
  outColor = texelFetch(u_src, ivec2(gl_FragCoord.xy), 0);\n\
}\n";
```

And near the other NV12 state:

```c
static GLuint nv12_copy_program;
static GLint nv12_copy_src_uniform;
static GLuint nv12_gpu[2];       /* [0] luma R8, [1] chroma RG8 */
static GLuint nv12_copy_fbo;
static int nv12_gpu_pitch, nv12_gpu_h;
```

- [ ] **Step 2: Build the copy program and GPU textures in `egl_init`**

Inside the `if (p_eglCreateImageKHR && p_glEGLImageTargetTexture2DOES)` block,
after `glGenTextures(2, nv12_texture);` add:

```c
    nv12_copy_program = compile_program(dmabuf_vertex_source,
                                        nv12_copy_fragment_source, "nv12-copy");
    nv12_copy_src_uniform = glGetUniformLocation(nv12_copy_program, "u_src");
    glGenTextures(2, nv12_gpu);
    glGenFramebuffers(1, &nv12_copy_fbo);
```

- [ ] **Step 3: Add the GPU-texture allocator**

Add next to `dmabuf_nv12_reset`:

```c
static void nv12_ensure_gpu_textures(int pitch, int height) {
  if (nv12_gpu_pitch == pitch && nv12_gpu_h == height)
    return;
  glBindTexture(GL_TEXTURE_2D, nv12_gpu[0]);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, pitch, height, 0, GL_RED,
               GL_UNSIGNED_BYTE, NULL);
  glBindTexture(GL_TEXTURE_2D, nv12_gpu[1]);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8, pitch / 2, height / 2, 0, GL_RG,
               GL_UNSIGNED_BYTE, NULL);
  nv12_gpu_pitch = pitch;
  nv12_gpu_h = height;
}
```

- [ ] **Step 4: In `egl_draw_dmabuf_nv12`, copy then display**

After the surface query and `nv12_ensure_gpu_textures(pitch, frame_height);`,
insert the copy pass:

```c
  /* Copy both external planes into GPU-native textures. */
  nv12_ensure_gpu_textures(pitch, frame_height);
  glBindFramebuffer(GL_FRAMEBUFFER, nv12_copy_fbo);
  glUseProgram(nv12_copy_program);
  glActiveTexture(GL_TEXTURE0);
  glUniform1i(nv12_copy_src_uniform, 0);
  glDisable(GL_BLEND);
  glDisableVertexAttribArray(0);

  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         nv12_gpu[0], 0);
  glBindTexture(GL_TEXTURE_2D, nv12_texture[0]);
  p_glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, img_y);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glViewport(0, 0, pitch, frame_height);
  glDrawArrays(GL_TRIANGLES, 0, 3);

  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         nv12_gpu[1], 0);
  glBindTexture(GL_TEXTURE_2D, nv12_texture[1]);
  p_glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, img_uv);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glViewport(0, 0, pitch / 2, frame_height / 2);
  glDrawArrays(GL_TRIANGLES, 0, 3);

  glBindFramebuffer(GL_FRAMEBUFFER, 0);
```

Then, in the existing display section, bind the **GPU textures** instead of the
EGLImages, restore the viewport, and set the int uniforms as today:

```c
  glUseProgram(nv12_program);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, nv12_gpu[0]);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, nv12_gpu[1]);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glViewport(0, 0, surf_w, surf_h);
  glUniform1i(nv12_uniforms[0], 0);
  glUniform1i(nv12_uniforms[1], 1);
  glUniform1i(nv12_uniforms[2], frame_width);
  glUniform1i(nv12_uniforms[3], frame_height);
  glUniform1i(nv12_uniforms[4], surf_w);
  glUniform1i(nv12_uniforms[5], surf_h);
  glUniform1i(nv12_trivial_uniform, dmabuf_trivial);
  upload_color_params(nv12_color_uniforms);
  glDisable(GL_BLEND);
  glDisableVertexAttribArray(0);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  glActiveTexture(GL_TEXTURE0);
  eglSwapBuffers(display, surface);
```

(The existing `EGL: nv12 two-plane import` debug line stays.)

- [ ] **Step 5: Build, self-test, measure**

```sh
scp F:/temp/wt-moonlight-drm/src/video/egl.c scy@172.30.71.3:~/Downloads/probe/moonlight-dev/src/video/egl.c
ssh scy@172.30.71.3 'sed -i "s/\r$//" ~/Downloads/probe/moonlight-dev/src/video/egl.c; cd ~/Downloads/probe/moonlight-dev && cmake --build build -j2'
# SELFTEST must still PASS, then:
ssh scy@172.30.71.3 'HOST=172.31.30.147 ML=$HOME/Downloads/probe/moonlight-dev/build/moonlight ~/Downloads/probe/h618-accept.sh; echo exit=$?'
```

Expected: SELFTEST PASSED; `default avg_c2 < 16.7 ms`, `avg_fps >= 58`.

- [ ] **Step 6: Commit if it meets the target; otherwise report**

If `c2 < 16.7 ms` and SELFTEST PASSED: commit `video/egl: copy the decoder buffer into GPU textures before display (Plan C)`.
If not: keep the tree clean (revert) and report the numbers — escalation to the kernel/DE spec is next.

---

## If Plan C fails

Report and stop. Next is the kernel DE33 VI-plane spec (hardware CSC + direct
scanout), which avoids GPU sampling entirely. Do not implement it in this plan.
