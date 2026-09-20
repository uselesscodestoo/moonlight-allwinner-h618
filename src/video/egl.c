/*
 * This file is part of Moonlight Embedded.
 *
 * Copyright (C) 2017 Iwan Timmer
 *
 * Moonlight is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * Moonlight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Moonlight; if not, see <http://www.gnu.org/licenses/>.
 */

#include "egl.h"

#include <Limelight.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES3/gl3ext.h>

#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

#ifdef __linux__
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#endif

#ifndef DRM_FORMAT_R8
#define DRM_FORMAT_R8 0x20203852 /* 'R', '8', ' ', ' ' */
#endif

#ifndef DRM_FORMAT_GR88
#define DRM_FORMAT_GR88 0x38385247 /* 'G', 'R', '8', '8' */
#endif

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

/* Not all GLES3 headers declare this extension entry point. */
typedef void (*moonlight_eglimage_target_texture2does)(GLenum target, void* image);

#define DMABUF_MAX_IMAGES 64

static const EGLint context_attributes[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
static const char* texture_mappings[] = { "ymap", "umap", "vmap" };

static const char* vertex_source = "\
#version 300 es\n\
in vec2 position;\n\
out mediump vec2 tex_position;\n\
void main() {\n\
  gl_Position = vec4(position, 0, 1);\n\
  tex_position = vec2((position.x + 1.) / 2., (1. - position.y) / 2.);\n\
}\n";

static const char* fragment_source = "\
#version 300 es\n\
precision mediump float;\n\
uniform lowp sampler2D ymap;\n\
uniform lowp sampler2D umap;\n\
uniform lowp sampler2D vmap;\n\
uniform float u_yscale;\n\
uniform float u_yoff;\n\
uniform float u_rv;\n\
uniform float u_gu;\n\
uniform float u_gv;\n\
uniform float u_bu;\n\
in mediump vec2 tex_position;\n\
out lowp vec4 fragColor;\n\
void main() {\n\
  mediump float y = texture(ymap, tex_position).r;\n\
  mediump float u = texture(umap, tex_position).r - 128.0 / 255.0;\n\
  mediump float v = texture(vmap, tex_position).r - 128.0 / 255.0;\n\
  mediump float yp = (y - u_yoff) * u_yscale;\n\
  lowp float r = yp + u_rv * v;\n\
  lowp float g = yp - u_gu * u - u_gv * v;\n\
  lowp float b = yp + u_bu * u;\n\
  fragColor = vec4(r, g, b, 1.0);\n\
}\n";

/* Zero-copy de-tile shaders: the decoder's tiled NV12 buffer is imported as a
 * single-plane R8 texture and untiled per-pixel in the fragment shader. */
static const char* dmabuf_vertex_source = "\
#version 300 es\n\
void main() {\n\
  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n\
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n\
}\n";

static const char* nv12_external_fragment_source = "\
#version 300 es\n\
#extension GL_OES_EGL_image_external_essl3 : require\n\
precision highp float;\n\
uniform samplerExternalOES u_tex;\n\
uniform vec2 u_view_size;\n\
layout(location = 0) out vec4 outColor;\n\
void main() {\n\
  vec2 uv = vec2(gl_FragCoord.x / u_view_size.x,\n\
                 1.0 - gl_FragCoord.y / u_view_size.y);\n\
  outColor = texture(u_tex, uv);\n\
}\n";

static const char* dmabuf_fragment_source = "\
#version 300 es\n\
precision highp float;\n\
precision highp int;\n\
precision highp sampler2D;\n\
uniform sampler2D u_tex;\n\
uniform int u_pitch;\n\
uniform int u_uv_offset;\n\
uniform int u_tpr;\n\
uniform int u_frame_w;\n\
uniform int u_frame_h;\n\
uniform int u_view_w;\n\
uniform int u_view_h;\n\
uniform int u_trivial;\n\
uniform float u_yscale;\n\
uniform float u_yoff;\n\
uniform float u_rv;\n\
uniform float u_gu;\n\
uniform float u_gv;\n\
uniform float u_bu;\n\
layout(location = 0) out vec4 outColor;\n\
int tiled_offset(int bc, int br) {\n\
  int band = br >> 5;\n\
  int trow = br & 31;\n\
  int tx   = bc >> 5;\n\
  int col  = bc & 31;\n\
  return band * (u_tpr * 1024) + tx * 1024 + trow * 32 + col;\n\
}\n\
float fetch_addr(int addr) {\n\
  int c = addr % u_pitch;\n\
  int r = addr / u_pitch;\n\
  return texelFetch(u_tex, ivec2(c, r), 0).r;\n\
}\n\
float fetch_plane(int off, int bc, int br) {\n\
  return fetch_addr(off + tiled_offset(bc, br));\n\
}\n\
void main() {\n\
  if (u_trivial != 0) { outColor = vec4(0.2, 0.4, 0.6, 1.0); return; }\n\
  int ux = (u_view_w == u_frame_w) ? int(gl_FragCoord.x)\n\
           : int(float(gl_FragCoord.x) * float(u_frame_w) / float(u_view_w));\n\
  int uy = (u_view_h == u_frame_h) ? int(gl_FragCoord.y)\n\
           : int(float(gl_FragCoord.y) * float(u_frame_h) / float(u_view_h));\n\
  int x = ux;\n\
  int y = u_frame_h - 1 - uy;\n\
  int cy = y >> 1;\n\
  int cx = x >> 1;\n\
  float Y = fetch_plane(0, x, y);\n\
  float U = fetch_plane(u_uv_offset, 2 * cx, cy);\n\
  float V = fetch_plane(u_uv_offset, 2 * cx + 1, cy);\n\
  float Yp = (Y - u_yoff) * u_yscale;\n\
  float Up = U - 128.0 / 255.0;\n\
  float Vp = V - 128.0 / 255.0;\n\
  vec3 rgb = vec3(Yp + u_rv * Vp,\n\
                  Yp - u_gu * Up - u_gv * Vp,\n\
                  Yp + u_bu * Up);\n\
  outColor = vec4(clamp(rgb, 0.0, 1.0), 1.0);\n\
}\n";

/* Linear NV12 path: the capture dma-buf is a plain NV12 buffer, so the luma
 * plane is imported as an R8 image and the interleaved chroma plane as a GR88
 * image (R = U, G = V).  Both planes are fetched with integer texelFetch, so
 * the shader needs just two fetches instead of three and no dependent chroma
 * double-fetch.  No de-tiling math is needed. */
static const char* nv12_fragment_source = "\
#version 300 es\n\
precision highp float;\n\
precision highp int;\n\
precision highp sampler2D;\n\
uniform sampler2D u_tex_y;\n\
uniform sampler2D u_tex_uv;\n\
uniform int u_frame_w;\n\
uniform int u_frame_h;\n\
uniform int u_view_w;\n\
uniform int u_view_h;\n\
uniform int u_trivial;\n\
uniform float u_yscale;\n\
uniform float u_yoff;\n\
uniform float u_rv;\n\
uniform float u_gu;\n\
uniform float u_gv;\n\
uniform float u_bu;\n\
layout(location = 0) out vec4 outColor;\n\
void main() {\n\
  if (u_trivial != 0) { outColor = vec4(0.2, 0.4, 0.6, 1.0); return; }\n\
  int ux = (u_view_w == u_frame_w) ? int(gl_FragCoord.x)\n\
           : int(float(gl_FragCoord.x) * float(u_frame_w) / float(u_view_w));\n\
  int uy = (u_view_h == u_frame_h) ? int(gl_FragCoord.y)\n\
           : int(float(gl_FragCoord.y) * float(u_frame_h) / float(u_view_h));\n\
  int x = ux;\n\
  int y = u_frame_h - 1 - uy;\n\
  int cx = x >> 1;\n\
  int cy = y >> 1;\n\
  float Y = texelFetch(u_tex_y, ivec2(x, y), 0).r;\n\
  vec2 UV = texelFetch(u_tex_uv, ivec2(cx, cy), 0).rg;\n\
  float Yp = (Y - u_yoff) * u_yscale;\n\
  float Up = UV.r - 128.0 / 255.0;\n\
  float Vp = UV.g - 128.0 / 255.0;\n\
  vec3 rgb = vec3(Yp + u_rv * Vp,\n\
                  Yp - u_gu * Up - u_gv * Vp,\n\
                  Yp + u_bu * Up);\n\
  outColor = vec4(clamp(rgb, 0.0, 1.0), 1.0);\n\
}\n";

static const float vertices[] = {
  -1.f,  1.f,
  -1.f, -1.f,
  1.f, -1.f,
  1.f, 1.f
};

static const GLuint elements[] = {
  0, 1, 2,
  2, 3, 0
};

static EGLDisplay display;
static EGLSurface surface;
static EGLContext context;

static int width, height;
static bool current;

static GLuint texture_id[3], texture_uniform[3];
static GLuint shader_program;
static GLuint vertex_vbo, index_ebo;

/* Zero-copy state */
static PFNEGLCREATEIMAGEKHRPROC p_eglCreateImageKHR;
static PFNEGLDESTROYIMAGEKHRPROC p_eglDestroyImageKHR;
static moonlight_eglimage_target_texture2does p_glEGLImageTargetTexture2DOES;
static GLuint dmabuf_program;
static GLuint nv12_program;
static GLint nv12_uniforms[6];
static GLint nv12_trivial_uniform;
static GLint nv12_color_uniforms[6];

/* YCbCr -> RGB conversion coefficients, set from the stream's own metadata
 * (HEVC VUI) by the caller; defaults to limited-range BT.601, which is what
 * Sunshine/NVENC desktop capture actually tags. */
static float color_yscale = 1.164383f, color_yoff = 16.0f / 255.0f;
static float color_rv = 1.596027f, color_gu = 0.391762f;
static float color_gv = 0.812968f, color_bu = 2.017232f;
static int color_use_bt709;
static int color_full_range;
static GLint sw_color_uniforms[6];
static GLuint dmabuf_texture;
static GLint dmabuf_uniforms[15];
static int dmabuf_trivial;
static struct {
  int fd;
  EGLImageKHR image;
} dmabuf_images[DMABUF_MAX_IMAGES];
static int dmabuf_images_count;
static int dmabuf_last_w, dmabuf_last_h;

static GLuint nv12_texture[2];   /* [0] luma R8, [1] chroma GR88 */
static struct {
  int fd;
  EGLImageKHR y;
  EGLImageKHR uv;
} dmabuf_nv12[DMABUF_MAX_IMAGES];
static int dmabuf_nv12_count;

static int nv12_external_requested, nv12_external_available;
static GLuint nv12_external_program, nv12_external_texture;
static GLint nv12_external_tex_uniform, nv12_external_view_uniform;
static int nv12_external_active_shown, nv12_external_draw_failures;
static int nv12_external_import_failures;
static struct {
  int fd, width, height, pitch, uv_offset;
  int use_bt709, full_range;
  EGLImageKHR image;
} dmabuf_nv12_external[DMABUF_MAX_IMAGES];
static int dmabuf_nv12_external_count;

static int zc_breakdown = -1;
static unsigned zc_bd_frames, zc_img_hits, zc_img_creates;
static double zc_bd_img, zc_bd_render, zc_bd_swap;

static double zc_now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}

static void zc_record_breakdown(double bd0, double bd1,
                                double bd2, double bd3) {
  if (!zc_breakdown)
    return;
  zc_bd_img += bd1 - bd0;
  zc_bd_render += bd2 - bd1;
  zc_bd_swap += bd3 - bd2;
  if (++zc_bd_frames == 30) {
    fprintf(stderr, "EGL: zc breakdown img=%.2fms render+finish=%.2fms swap=%.2fms "
                    "creates=%u hits=%u\n",
            zc_bd_img / 30, zc_bd_render / 30, zc_bd_swap / 30,
            zc_img_creates, zc_img_hits);
    zc_bd_img = zc_bd_render = zc_bd_swap = 0;
    zc_bd_frames = 0;
  }
}

static int has_gl_extension(const char* wanted) {
  GLint count = 0;
  glGetIntegerv(GL_NUM_EXTENSIONS, &count);
  for (GLint i = 0; i < count; i++) {
    const char* extension = (const char*)glGetStringi(GL_EXTENSIONS, i);
    if (extension && strcmp(extension, wanted) == 0)
      return 1;
  }
  return 0;
}

static EGLImageKHR create_single_plane_image(int fd, int fourcc, int pitch,
                                             int offset, int height) {
  EGLint attrs[] = {
    EGL_WIDTH,  (fourcc == DRM_FORMAT_GR88) ? pitch / 2 : pitch,
    EGL_HEIGHT, height,
    EGL_LINUX_DRM_FOURCC_EXT, fourcc,
    EGL_DMA_BUF_PLANE0_FD_EXT, fd,
    EGL_DMA_BUF_PLANE0_OFFSET_EXT, offset,
    EGL_DMA_BUF_PLANE0_PITCH_EXT, pitch,
    EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, 0,
    EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, 0,
    EGL_NONE
  };
  return p_eglCreateImageKHR(display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT,
                             NULL, attrs);
}

static int dmabuf_get_nv12(int fd, int height, int pitch,
                           int uv_offset, EGLImageKHR *out_y, EGLImageKHR *out_uv) {
  int i;
  for (i = 0; i < dmabuf_nv12_count; i++)
    if (dmabuf_nv12[i].fd == fd) {
      *out_y = dmabuf_nv12[i].y;
      *out_uv = dmabuf_nv12[i].uv;
      zc_img_hits++;
      return (dmabuf_nv12[i].y != EGL_NO_IMAGE_KHR &&
              dmabuf_nv12[i].uv != EGL_NO_IMAGE_KHR) ? 0 : -1;
    }
  if (dmabuf_nv12_count >= DMABUF_MAX_IMAGES)
    return -1;
  EGLImageKHR y  = create_single_plane_image(fd, DRM_FORMAT_R8, pitch, 0, height);
  EGLImageKHR uv = create_single_plane_image(fd, DRM_FORMAT_GR88, pitch,
                                             uv_offset, height / 2);
  if (y == EGL_NO_IMAGE_KHR || uv == EGL_NO_IMAGE_KHR) {
    static int failures;
    if (failures++ < 5)
      fprintf(stderr, "EGL: nv12 image(fd=%d) failed y=%p uv=%p 0x%x\n",
              fd, (void *)y, (void *)uv, eglGetError());
    if (y  != EGL_NO_IMAGE_KHR && p_eglDestroyImageKHR) p_eglDestroyImageKHR(display, y);
    if (uv != EGL_NO_IMAGE_KHR && p_eglDestroyImageKHR) p_eglDestroyImageKHR(display, uv);
    /* Negative cache: without this a persistently bad fd would retry two
     * eglCreateImageKHR calls on every single frame.  The hit path above
     * returns -1 immediately for an entry whose images are EGL_NO_IMAGE_KHR. */
    if (dmabuf_nv12_count < DMABUF_MAX_IMAGES) {
      dmabuf_nv12[dmabuf_nv12_count].fd = fd;
      dmabuf_nv12[dmabuf_nv12_count].y  = EGL_NO_IMAGE_KHR;
      dmabuf_nv12[dmabuf_nv12_count].uv = EGL_NO_IMAGE_KHR;
      dmabuf_nv12_count++;
      zc_img_creates++;
    }
    return -1;
  }
  dmabuf_nv12[dmabuf_nv12_count].fd = fd;
  dmabuf_nv12[dmabuf_nv12_count].y  = y;
  dmabuf_nv12[dmabuf_nv12_count].uv = uv;
  dmabuf_nv12_count++;
  zc_img_creates++;
  *out_y = y;
  *out_uv = uv;
  return 0;
}

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
    EGL_YUV_COLOR_SPACE_HINT_EXT, color_use_bt709 ? EGL_ITU_REC709_EXT : EGL_ITU_REC601_EXT,
    EGL_SAMPLE_RANGE_HINT_EXT, color_full_range ? EGL_YUV_FULL_RANGE_EXT : EGL_YUV_NARROW_RANGE_EXT,
    EGL_YUV_CHROMA_HORIZONTAL_SITING_HINT_EXT, EGL_YUV_CHROMA_SITING_0_EXT,
    EGL_YUV_CHROMA_VERTICAL_SITING_HINT_EXT, EGL_YUV_CHROMA_SITING_0_EXT,
    EGL_NONE
  };
  EGLImageKHR image = p_eglCreateImageKHR(display, EGL_NO_CONTEXT,
                                         EGL_LINUX_DMA_BUF_EXT, NULL, attrs);
  if (image == EGL_NO_IMAGE_KHR && nv12_external_import_failures < 5) {
    nv12_external_import_failures++;
    fprintf(stderr, "EGL: nv12 external image(fd=%d) failed 0x%x\n", fd, eglGetError());
  }

  /* Keep failed imports too, so a rejected layout is not retried each frame. */
  int i = dmabuf_nv12_external_count++;
  dmabuf_nv12_external[i].fd = fd;
  dmabuf_nv12_external[i].width = frame_width;
  dmabuf_nv12_external[i].height = frame_height;
  dmabuf_nv12_external[i].pitch = pitch;
  dmabuf_nv12_external[i].uv_offset = uv_offset;
  dmabuf_nv12_external[i].use_bt709 = color_use_bt709;
  dmabuf_nv12_external[i].full_range = color_full_range;
  dmabuf_nv12_external[i].image = image;
  zc_img_creates++;
  return image;
}

static void dmabuf_nv12_external_reset(void) {
  for (int i = 0; i < dmabuf_nv12_external_count; i++) {
    if (p_eglDestroyImageKHR && dmabuf_nv12_external[i].image != EGL_NO_IMAGE_KHR)
      p_eglDestroyImageKHR(display, dmabuf_nv12_external[i].image);
  }
  dmabuf_nv12_external_count = 0;
}

static void dmabuf_nv12_reset(void) {
  int i;
  for (i = 0; i < dmabuf_nv12_count; i++) {
    if (p_eglDestroyImageKHR) {
      if (dmabuf_nv12[i].y  != EGL_NO_IMAGE_KHR) p_eglDestroyImageKHR(display, dmabuf_nv12[i].y);
      if (dmabuf_nv12[i].uv != EGL_NO_IMAGE_KHR) p_eglDestroyImageKHR(display, dmabuf_nv12[i].uv);
    }
  }
  dmabuf_nv12_count = 0;
}

void egl_set_color_params(float yscale, float yoff, float rv, float gu, float gv, float bu) {
  color_yscale = yscale;
  color_yoff = yoff;
  color_rv = rv;
  color_gu = gu;
  color_gv = gv;
  color_bu = bu;
}

void egl_set_color_mode(int use_bt709, int full_range) {
  color_use_bt709 = !!use_bt709;
  color_full_range = !!full_range;
}

static void upload_color_params(const GLint* u) {
  glUniform1f(u[0], color_yscale);
  glUniform1f(u[1], color_yoff);
  glUniform1f(u[2], color_rv);
  glUniform1f(u[3], color_gu);
  glUniform1f(u[4], color_gv);
  glUniform1f(u[5], color_bu);
}


/* Unit test for the YCbCr -> RGB conversion: feed known limited-range BT.601
 * triples through the very same shader the stream path uses (an R8 luma
 * texture and a GR88 chroma texture) and compare the rendered pixels against
 * the standard's expected RGB.  Enabled with MOONLIGHT_COLOR_SELFTEST=1; runs
 * once, before the first stream frame. */
static void color_selftest(void) {
  static const struct {
    const char* name;
    unsigned char y, u, v;
    int r, g, b;
  } cases[] = {
    { "limited white", 235, 128, 128, 255, 255, 255 },
    { "limited black",  16, 128, 128,   0,   0,   0 },
    { "limited grey",  126, 128, 128, 128, 128, 128 },
    { "red",            81,  90, 240, 255,   0,   0 },
    { "green",         145,  54,  34,   0, 255,   1 },
    { "blue",           41, 240, 110,   0,   0, 255 },
  };
  const int w = 4, h = 4;
  unsigned char luma[4 * 4];
  unsigned char chroma[2 * 2 * 2];
  unsigned char pixels[4 * 4 * 4];
  GLuint tex_y, tex_uv, fbo, rbo;
  int failures = 0;
  unsigned int c;

  if (!nv12_program)
    return;

  glGenTextures(1, &tex_y);
  glGenTextures(1, &tex_uv);
  glGenFramebuffers(1, &fbo);
  glGenRenderbuffers(1, &rbo);

  glBindRenderbuffer(GL_RENDERBUFFER, rbo);
  glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, w, h);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rbo);
  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
    fprintf(stderr, "SELFTEST: framebuffer incomplete, skipped\n");
    goto out;
  }

  glUseProgram(nv12_program);
  glUniform1i(nv12_uniforms[2], w);      /* u_frame_w */
  glUniform1i(nv12_uniforms[3], h);      /* u_frame_h */
  glUniform1i(nv12_uniforms[4], w);      /* u_view_w  */
  glUniform1i(nv12_uniforms[5], h);      /* u_view_h  */
  glUniform1i(nv12_trivial_uniform, 0);

  glViewport(0, 0, w, h);
  glDisable(GL_BLEND);
  glDisableVertexAttribArray(0);

  egl_set_color_params(1.164383f, 16.0f / 255.0f, 1.596027f, 0.391762f, 0.812968f, 2.017232f);
  upload_color_params(nv12_color_uniforms);

  fprintf(stderr, "SELFTEST: limited-range BT.601 conversion (frame %dx%d)\n", w, h);

  for (c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
    int x, y, i, ok = 1;

    for (y = 0; y < h; y++)
      for (x = 0; x < w; x++)
        luma[y * w + x] = cases[c].y;
    for (y = 0; y < h / 2; y++)
      for (x = 0; x < w / 2; x++) {
        chroma[(y * (w / 2) + x) * 2 + 0] = cases[c].u;
        chroma[(y * (w / 2) + x) * 2 + 1] = cases[c].v;
      }

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex_y);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, w, h, 0, GL_RED, GL_UNSIGNED_BYTE, luma);
    glUniform1i(nv12_uniforms[0], 0);

    /* The chroma plane is uploaded as a plain GL_RG8 texture, not a real
     * GR88 dma-buf: this validates the shader's .rg mapping and the YCbCr
     * maths, but not the byte order the EGL dma-buf import produces. */
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, tex_uv);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8, w / 2, h / 2, 0, GL_RG,
                 GL_UNSIGNED_BYTE, chroma);
    glUniform1i(nv12_uniforms[1], 1);

    glDrawArrays(GL_TRIANGLES, 0, 3);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels);

    for (i = 0; i < w * h; i++) {
      int r = pixels[i * 4 + 0], g = pixels[i * 4 + 1], b = pixels[i * 4 + 2];
      if (abs(r - cases[c].r) > 3 || abs(g - cases[c].g) > 3 || abs(b - cases[c].b) > 3) {
        fprintf(stderr, "SELFTEST: %s Y=%u U=%u V=%u -> got (%d,%d,%d), expected (%d,%d,%d)\n",
                cases[c].name, cases[c].y, cases[c].u, cases[c].v, r, g, b,
                cases[c].r, cases[c].g, cases[c].b);
        ok = 0;
        break;
      }
    }
    if (ok)
      fprintf(stderr, "SELFTEST: %-14s OK\n", cases[c].name);
    else
      failures++;
  }

  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  fprintf(stderr, "SELFTEST: %s\n", failures ? "FAILED" : "PASSED");

out:
  glDeleteRenderbuffers(1, &rbo);
  glDeleteFramebuffers(1, &fbo);
  glDeleteTextures(1, &tex_y);
  glDeleteTextures(1, &tex_uv);
  glViewport(0, 0, width, height);
}

static GLuint compile_program(const char* vs_src, const char* fs_src, const char* name) {
  GLuint vs = glCreateShader(GL_VERTEX_SHADER);
  GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
  GLuint prog;
  GLint ok = 0;

  glShaderSource(vs, 1, &vs_src, NULL);
  glShaderSource(fs, 1, &fs_src, NULL);
  glCompileShader(vs);
  glCompileShader(fs);

  glGetShaderiv(fs, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[2048];
    glGetShaderInfoLog(fs, sizeof(log), NULL, log);
    fprintf(stderr, "EGL: %s fragment shader failed:\n%s\n", name, log);
  }
  glGetShaderiv(vs, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[2048];
    glGetShaderInfoLog(vs, sizeof(log), NULL, log);
    fprintf(stderr, "EGL: %s vertex shader failed:\n%s\n", name, log);
  }

  prog = glCreateProgram();
  glAttachShader(prog, vs);
  glAttachShader(prog, fs);
  glBindAttribLocation(prog, 0, "position");
  glLinkProgram(prog);
  glDeleteShader(vs);
  glDeleteShader(fs);

  ok = 0;
  glGetProgramiv(prog, GL_LINK_STATUS, &ok);
  if (!ok) {
    char log[2048];
    glGetProgramInfoLog(prog, sizeof(log), NULL, log);
    fprintf(stderr, "EGL: %s program link failed:\n%s\n", name, log);
    glDeleteProgram(prog);
    return 0;
  }

  return prog;
}

void egl_init(EGLNativeDisplayType native_display, NativeWindowType native_window, int display_width, int display_height) {
  width = display_width;
  height = display_height;

  display = eglGetDisplay(native_display);
  if (display == EGL_NO_DISPLAY) {
    fprintf( stderr, "EGL: error get display\n" );
    exit(EXIT_FAILURE);
  }

  int major, minor;
  EGLBoolean result = eglInitialize(display, &major, &minor);
  if (result == EGL_FALSE) {
    fprintf( stderr, "EGL: error initialising display\n");
    exit(EXIT_FAILURE);
  }

  EGLConfig config = NULL;
  static const EGLint attribute_list[] = { EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_NONE };

  EGLint totalConfigsFound = 0;
  result = eglChooseConfig(display, attribute_list, &config, 1, &totalConfigsFound);
  if (result != EGL_TRUE || totalConfigsFound == 0) {
    fprintf(stderr, "EGL: Unable to query for available configs, found %d.\n", totalConfigsFound);
    exit(EXIT_FAILURE);
  }

  result = eglBindAPI(EGL_OPENGL_ES_API);
  if (result == EGL_FALSE) {
    fprintf(stderr, "EGL: error binding API\n");
    exit(EXIT_FAILURE);
  }

  context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attributes);
  if (context == EGL_NO_CONTEXT) {
    fprintf(stderr, "EGL: couldn't get a valid context\n");
    exit(EXIT_FAILURE);
  }

  surface = eglCreateWindowSurface(display, config, (NativeWindowType) native_window, NULL);
  eglMakeCurrent(display, surface, surface, context);
  /* Swap interval defaults to 1 (wait for a buffer to come back from the
   * server); MOONLIGHT_NO_VSYNC=1 unthrottles presentation for benchmarking. */
  if (getenv("MOONLIGHT_NO_VSYNC"))
    eglSwapInterval(display, 0);
  fprintf(stderr, "EGL: renderer=%s version=%s GLSL=%s surface=%dx%d\n",
          glGetString(GL_RENDERER), glGetString(GL_VERSION),
          glGetString(GL_SHADING_LANGUAGE_VERSION), width, height);
  fprintf(stderr, "EGL: vsync=%s\n", getenv("MOONLIGHT_NO_VSYNC") ? "off" : "on");

  GLuint vbo;
  glGenBuffers(1, &vbo);
  glBindBuffer(GL_ARRAY_BUFFER, vbo);
  glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
  vertex_vbo = vbo;

  GLuint ebo;
  glGenBuffers(1, &ebo);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
  glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(elements), elements, GL_STATIC_DRAW);
  index_ebo = ebo;

  shader_program = compile_program(vertex_source, fragment_source, "yuv420p");
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(GLfloat), 0);

  glGenTextures(3, texture_id);
  for (int i = 0; i < 3; i++) {
    glBindTexture(GL_TEXTURE_2D, texture_id[i]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, i > 0 ? width / 2 : width, i > 0 ? height / 2 : height, 0, GL_RED, GL_UNSIGNED_BYTE, 0);
    texture_uniform[i] = glGetUniformLocation(shader_program, texture_mappings[i]);
  }
  {
    static const char* names[6] = { "u_yscale", "u_yoff", "u_rv", "u_gu", "u_gv", "u_bu" };
    for (int i = 0; i < 6; i++)
      sw_color_uniforms[i] = glGetUniformLocation(shader_program, names[i]);
  }

  p_eglCreateImageKHR = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
  p_eglDestroyImageKHR = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
  p_glEGLImageTargetTexture2DOES = (moonlight_eglimage_target_texture2does)eglGetProcAddress("glEGLImageTargetTexture2DOES");

  nv12_external_requested = getenv("MOONLIGHT_ZC_EXTERNAL") != NULL;
  nv12_external_available = 0;
  nv12_external_active_shown = 0;
  nv12_external_draw_failures = 0;
  nv12_external_import_failures = 0;
  if (nv12_external_requested) {
    if (getenv("MOONLIGHT_ZC_COMPUTE")) {
      fprintf(stderr, "EGL: MOONLIGHT_ZC_EXTERNAL and MOONLIGHT_ZC_COMPUTE are mutually exclusive; disabling external path\n");
    } else if (p_eglCreateImageKHR && p_eglDestroyImageKHR &&
               p_glEGLImageTargetTexture2DOES &&
               has_gl_extension("GL_OES_EGL_image_external") &&
               has_gl_extension("GL_OES_EGL_image_external_essl3")) {
      nv12_external_program = compile_program(dmabuf_vertex_source,
                                               nv12_external_fragment_source,
                                               "nv12-external");
      if (nv12_external_program) {
        nv12_external_tex_uniform = glGetUniformLocation(nv12_external_program, "u_tex");
        nv12_external_view_uniform = glGetUniformLocation(nv12_external_program, "u_view_size");
        glGenTextures(1, &nv12_external_texture);
        nv12_external_available = nv12_external_texture != 0;
      }
    }
    if (!nv12_external_available)
      fprintf(stderr, "EGL: native NV12 external path unavailable; using two-plane path\n");
  }

  if (p_eglCreateImageKHR && p_glEGLImageTargetTexture2DOES) {
    dmabuf_program = compile_program(dmabuf_vertex_source, dmabuf_fragment_source, "dmabuf-detile");
    dmabuf_uniforms[0] = glGetUniformLocation(dmabuf_program, "u_tex");
    dmabuf_uniforms[1] = glGetUniformLocation(dmabuf_program, "u_pitch");
    dmabuf_uniforms[2] = glGetUniformLocation(dmabuf_program, "u_uv_offset");
    dmabuf_uniforms[3] = glGetUniformLocation(dmabuf_program, "u_tpr");
    dmabuf_uniforms[4] = glGetUniformLocation(dmabuf_program, "u_frame_w");
    dmabuf_uniforms[5] = glGetUniformLocation(dmabuf_program, "u_frame_h");
    dmabuf_uniforms[6] = glGetUniformLocation(dmabuf_program, "u_view_w");
    dmabuf_uniforms[7] = glGetUniformLocation(dmabuf_program, "u_view_h");
    static const char* cnames[6] = { "u_yscale", "u_yoff", "u_rv", "u_gu", "u_gv", "u_bu" };
    dmabuf_uniforms[8] = glGetUniformLocation(dmabuf_program, "u_trivial");
    for (int i = 0; i < 6; i++)
      dmabuf_uniforms[9 + i] = glGetUniformLocation(dmabuf_program, cnames[i]);
    dmabuf_trivial = getenv("MOONLIGHT_ZC_TRIVIAL") != NULL;
    glGenTextures(1, &dmabuf_texture);
    glGenTextures(2, nv12_texture);

    nv12_program = compile_program(dmabuf_vertex_source, nv12_fragment_source, "nv12-linear");
    nv12_uniforms[0] = glGetUniformLocation(nv12_program, "u_tex_y");
    nv12_uniforms[1] = glGetUniformLocation(nv12_program, "u_tex_uv");
    nv12_uniforms[2] = glGetUniformLocation(nv12_program, "u_frame_w");
    nv12_uniforms[3] = glGetUniformLocation(nv12_program, "u_frame_h");
    nv12_uniforms[4] = glGetUniformLocation(nv12_program, "u_view_w");
    nv12_uniforms[5] = glGetUniformLocation(nv12_program, "u_view_h");
    nv12_trivial_uniform = glGetUniformLocation(nv12_program, "u_trivial");
    {
      static const char* cnames[6] = { "u_yscale", "u_yoff", "u_rv", "u_gu", "u_gv", "u_bu" };
      for (int i = 0; i < 6; i++)
        nv12_color_uniforms[i] = glGetUniformLocation(nv12_program, cnames[i]);
    }
  } else {
    fprintf(stderr, "EGL: dmabuf import extensions unavailable\n");
    dmabuf_program = 0;
  }

  if (getenv("MOONLIGHT_COLOR_SELFTEST"))
    color_selftest();

  eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  current = false;
}

void egl_draw(uint8_t* image[3]) {
  if (!current) {
    eglMakeCurrent(display, surface, surface, context);
    current = true;
  }

  glUseProgram(shader_program);
  glBindBuffer(GL_ARRAY_BUFFER, vertex_vbo);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, index_ebo);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(GLfloat), 0);

  upload_color_params(sw_color_uniforms);

  for (int i = 0; i < 3; i++) {
    glActiveTexture(GL_TEXTURE0 + i);
    glBindTexture(GL_TEXTURE_2D, texture_id[i]);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, i > 0 ? width / 2 : width, i > 0 ? height / 2 : height, GL_RED, GL_UNSIGNED_BYTE, image[i]);
    glUniform1i(texture_uniform[i], i);
  }

  glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);

  eglSwapBuffers(display, surface);
}

static EGLImageKHR dmabuf_get_image(int fd, int byte_pitch, int tex_height) {
  int i;

  for (i = 0; i < dmabuf_images_count; i++)
    if (dmabuf_images[i].fd == fd)
      return dmabuf_images[i].image;

  if (dmabuf_images_count >= DMABUF_MAX_IMAGES)
    return EGL_NO_IMAGE_KHR;

  EGLint attrs[] = {
    EGL_WIDTH, byte_pitch,
    EGL_HEIGHT, tex_height,
    EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_R8,
    EGL_DMA_BUF_PLANE0_FD_EXT, fd,
    EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
    EGL_DMA_BUF_PLANE0_PITCH_EXT, byte_pitch,
    EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, 0,
    EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, 0,
    EGL_NONE
  };
  EGLImageKHR img = p_eglCreateImageKHR(display, EGL_NO_CONTEXT,
                                        EGL_LINUX_DMA_BUF_EXT, NULL, attrs);
  if (img == EGL_NO_IMAGE_KHR) {
    static int failures;
    if (failures++ < 5)
      fprintf(stderr, "EGL: eglCreateImageKHR(fd=%d) failed 0x%x\n", fd, eglGetError());
    return EGL_NO_IMAGE_KHR;
  }

  dmabuf_images[dmabuf_images_count].fd = fd;
  dmabuf_images[dmabuf_images_count].image = img;
  dmabuf_images_count++;
  return img;
}

int egl_draw_dmabuf(int dmabuf_fd, unsigned int size, int frame_width, int frame_height,
                    int uv_offset, int byte_pitch) {
  EGLImageKHR img;
  int tex_height;
  EGLint surf_w = 0, surf_h = 0;

  if (!dmabuf_program)
    return -1;

  if (!current) {
    eglMakeCurrent(display, surface, surface, context);
    current = true;
  }

  /* Resolution change invalidates all cached images/fds. */
  if (frame_width != dmabuf_last_w || frame_height != dmabuf_last_h) {
    for (int i = 0; i < dmabuf_images_count; i++)
      if (p_eglDestroyImageKHR)
        p_eglDestroyImageKHR(display, dmabuf_images[i].image);
    dmabuf_images_count = 0;
    dmabuf_last_w = frame_width;
    dmabuf_last_h = frame_height;
  }

  tex_height = ((int)size + byte_pitch - 1) / byte_pitch;
  img = dmabuf_get_image(dmabuf_fd, byte_pitch, tex_height);
  if (img == EGL_NO_IMAGE_KHR)
    return -1;

  /* The EGL surface is the real viewport: the window may be scaled by the WM
   * (fullscreen) and differs from the stream resolution. */
  eglQuerySurface(display, surface, EGL_WIDTH, &surf_w);
  eglQuerySurface(display, surface, EGL_HEIGHT, &surf_h);
  if (surf_w <= 0)
    surf_w = width;
  if (surf_h <= 0)
    surf_h = height;

  glUseProgram(dmabuf_program);

  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, dmabuf_texture);
  p_glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, img);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  glUniform1i(dmabuf_uniforms[0], 0);
  glUniform1i(dmabuf_uniforms[1], byte_pitch);
  glUniform1i(dmabuf_uniforms[2], uv_offset);
  glUniform1i(dmabuf_uniforms[3], (frame_width + 31) / 32);
  glUniform1i(dmabuf_uniforms[4], frame_width);
  glUniform1i(dmabuf_uniforms[5], frame_height);
  glUniform1i(dmabuf_uniforms[6], surf_w);
  glUniform1i(dmabuf_uniforms[7], surf_h);
  glUniform1i(dmabuf_uniforms[8], dmabuf_trivial);
  upload_color_params(&dmabuf_uniforms[9]);

  {
    static int dbg_shown;
    if (!dbg_shown) {
      fprintf(stderr, "EGL: dmabuf import fd=%d size=%u pitch=%d tex_h=%d uv_off=%d "
                      "frame=%dx%d view=%dx%d tpr=%d\n",
              dmabuf_fd, size, byte_pitch, tex_height, uv_offset,
              frame_width, frame_height, surf_w, surf_h, (frame_width + 31) / 32);
      dbg_shown = 1;
    }
  }

  glDisable(GL_BLEND);
  glDisableVertexAttribArray(0);
  glDrawArrays(GL_TRIANGLES, 0, 3);

  eglSwapBuffers(display, surface);
  return 0;
}

static int draw_dmabuf_nv12_external(int fd, unsigned int size,
                                     int frame_width, int frame_height,
                                     int pitch, int uv_offset,
                                     int surf_w, int surf_h,
                                     double* import_done) {
  if (!nv12_external_available || fd < 0 || frame_width <= 0 || frame_height <= 0 ||
      surf_w <= 0 || surf_h <= 0 || pitch <= 0 || pitch < frame_width ||
      (uint64_t)pitch < 2 * (((uint64_t)frame_width + 1) / 2) ||
      uv_offset < 0 || uv_offset % pitch != 0 ||
      (uint64_t)uv_offset < (uint64_t)pitch * frame_height)
    return -1;

  uint64_t required = (uint64_t)uv_offset +
                      (uint64_t)pitch * (((uint64_t)frame_height + 1) / 2);
  if (required > size)
    return -1;

  EGLImageKHR image = dmabuf_get_nv12_external(fd, frame_width, frame_height,
                                              pitch, uv_offset);
  if (import_done)
    *import_done = zc_now_ms();
  if (image == EGL_NO_IMAGE_KHR)
    return -1;

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
  glUniform2f(nv12_external_view_uniform, (GLfloat)surf_w, (GLfloat)surf_h);
  glViewport(0, 0, surf_w, surf_h);
  glDisable(GL_BLEND);
  glDisableVertexAttribArray(0);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
  return glGetError() == GL_NO_ERROR ? 0 : -1;
}

#ifdef __linux__
static int alloc_cma_dmabuf(size_t size, void** map_out) {
  int heap = open("/dev/dma_heap/default_cma_region", O_RDWR | O_CLOEXEC);
  struct dma_heap_allocation_data alloc = {0};
  if (heap < 0) {
    perror("SELFTEST: open CMA heap");
    return -1;
  }
  alloc.len = size;
  alloc.fd_flags = O_RDWR | O_CLOEXEC;
  if (ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &alloc) < 0) {
    perror("SELFTEST: allocate CMA dma-buf");
    close(heap);
    return -1;
  }
  close(heap);
  *map_out = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, (int)alloc.fd, 0);
  if (*map_out == MAP_FAILED) {
    perror("SELFTEST: map CMA dma-buf");
    close((int)alloc.fd);
    return -1;
  }
  return (int)alloc.fd;
}

static int dmabuf_sync(int fd, uint64_t flags) {
  struct dma_buf_sync sync = { .flags = flags };
  return ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
}

static int external_check_pixel(const char* name, const uint8_t* pixels,
                                int w, int x, int y, int r, int g, int b) {
  const uint8_t* pixel = pixels + 4 * (y * w + x);
  if (abs((int)pixel[0] - r) <= 3 && abs((int)pixel[1] - g) <= 3 &&
      abs((int)pixel[2] - b) <= 3)
    return 0;
  fprintf(stderr, "SELFTEST: %s pixel (%d,%d) FAIL got=(%d,%d,%d) expected=(%d,%d,%d)\n",
          name, x, y, pixel[0], pixel[1], pixel[2], r, g, b);
  return -1;
}
#endif

/* Exercise the same import, color hints, texture sampling and orientation as
 * streaming, without a swap or a fallback to the two-plane renderer. */
int egl_nv12_external_selftest(void) {
#ifndef __linux__
  printf("SELFTEST: NV12 external SKIP (non-Linux)\n");
  return 77;
#else
  static const struct external_case {
    const char* name;
    int use709, full;
    uint8_t y, u, v;
    uint8_t expect_r, expect_g, expect_b;
  } external_cases[] = {
    { "601 narrow black", 0,0, 16,128,128, 0,0,0 },
    { "601 narrow white", 0,0, 235,128,128, 255,255,255 },
    { "601 narrow grey",  0,0, 126,128,128, 128,128,128 },
    { "601 narrow red",   0,0, 81,90,240, 255,0,0 },
    { "709 narrow red",   1,0, 63,102,240, 255,1,0 },
    { "601 full grey",    0,1, 128,128,128, 128,128,128 },
    { "601 full dark", 0,1, 64,128,128, 64,64,64 },
  };
  /* Source order: top-left, top-right, bottom-left, bottom-right. */
  static const uint8_t quadrant_yuv[4][3] = {
    {81,90,240}, {145,54,34}, {41,240,110}, {126,128,128}
  };
  static const uint8_t quadrant_rgb[4][3] = {
    {255,0,0}, {0,255,1}, {0,0,255}, {128,128,128}
  };
  static const int quadrant_probes[][2] = {
    {16,48}, {48,48}, {16,16}, {48,16},
    {31,16}, {32,16}, {31,48}, {32,48},
    {16,31}, {16,32}, {48,31}, {48,32}
  };
  const int constant_count = sizeof(external_cases) / sizeof(external_cases[0]);
  const size_t buffer_size = 6144;
  uint8_t pixels[64 * 64 * 4]; /* Also holds the 1280x2 scaled output. */
  void* map = MAP_FAILED;
  int fd = -1, cpu_writing = 0, rc = 1;
  const char* skip_reason = NULL;
  GLuint fbo = 0, rbo = 0;
  GLint previous_draw_fbo, previous_read_fbo, previous_rbo, previous_viewport[4];
  int previous_use709 = color_use_bt709, previous_full = color_full_range;
  EGLDisplay previous_display = EGL_NO_DISPLAY;
  EGLContext previous_context = EGL_NO_CONTEXT;
  EGLSurface previous_draw_surface = EGL_NO_SURFACE, previous_read_surface = EGL_NO_SURFACE;
  bool acquired_context = false;

  if (!nv12_external_available) {
    printf("SELFTEST: NV12 external SKIP (extension or shader unavailable)\n");
    return 77;
  }
  if (!current) {
    previous_display = eglGetCurrentDisplay();
    previous_context = eglGetCurrentContext();
    previous_draw_surface = eglGetCurrentSurface(EGL_DRAW);
    previous_read_surface = eglGetCurrentSurface(EGL_READ);
    if (!eglMakeCurrent(display, surface, surface, context)) {
      printf("SELFTEST: NV12 external SKIP (EGL context unavailable)\n");
      return 77;
    }
    current = true;
    acquired_context = true;
  }

  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previous_draw_fbo);
  glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previous_read_fbo);
  glGetIntegerv(GL_RENDERBUFFER_BINDING, &previous_rbo);
  glGetIntegerv(GL_VIEWPORT, previous_viewport);
  glGenFramebuffers(1, &fbo);
  glGenRenderbuffers(1, &rbo);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  glBindRenderbuffer(GL_RENDERBUFFER, rbo);
  glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, 64, 64);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rbo);
  if (!fbo || !rbo || glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE ||
      glGetError() != GL_NO_ERROR) {
    fprintf(stderr, "SELFTEST: RGBA8 framebuffer setup FAIL\n");
    goto cleanup;
  }

  fd = alloc_cma_dmabuf(buffer_size, &map);
  if (fd < 0) {
    skip_reason = "CMA dma-buf allocation or mapping unavailable";
    rc = 77;
    goto cleanup;
  }

  for (int c = 0; c < constant_count + 2; c++) {
    int quadrants = c == constant_count;
    int scaled = c == constant_count + 1;
    const struct external_case* test = c < constant_count ? &external_cases[c] : NULL;
    const char* name = test ? test->name : quadrants ? "quadrants" : "scaled 1920->1280 detail";
    int frame_w = scaled ? 1920 : 64, frame_h = scaled ? 2 : 64;
    int view_w = scaled ? 1280 : 64, view_h = frame_h;
    int uv_offset = scaled ? 3840 : 4096;
    uint8_t* data = map;

    egl_set_color_mode(test ? test->use709 : 0, test ? test->full : 0);
    dmabuf_nv12_external_reset();
    if (dmabuf_sync(fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE) < 0) {
      perror("SELFTEST: DMA_BUF_SYNC_START WRITE");
      skip_reason = "DMA_BUF_SYNC_START WRITE unavailable";
      rc = 77;
      goto cleanup;
    }
    cpu_writing = 1;
    if (test) {
      memset(data, test->y, 4096);
      for (int i = 4096; i < 6144; i += 2) {
        data[i] = test->u;
        data[i + 1] = test->v;
      }
    } else if (quadrants) {
      for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++)
          data[y * 64 + x] = quadrant_yuv[2 * (y >= 32) + (x >= 32)][0];
      for (int y = 0; y < 32; y++) {
        for (int x = 0; x < 64; x += 2) {
          int q = 2 * (y >= 16) + (x >= 32);
          data[4096 + y * 64 + x] = quadrant_yuv[q][1];
          data[4096 + y * 64 + x + 1] = quadrant_yuv[q][2];
        }
      }
    } else {
      for (int y = 0; y < 2; y++)
        for (int x = 0; x < 1920; x++)
          data[y * 1920 + x] = (x & 1) ? 235 : 16;
      memset(data + 3840, 128, 1920);
    }
    if (dmabuf_sync(fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE) < 0) {
      perror("SELFTEST: DMA_BUF_SYNC_END WRITE");
      skip_reason = "DMA_BUF_SYNC_END WRITE unavailable";
      rc = 77;
      goto cleanup;
    }
    cpu_writing = 0;

    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    if (scaled) {
      glBindRenderbuffer(GL_RENDERBUFFER, rbo);
      glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, view_w, view_h);
    }
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE ||
        glGetError() != GL_NO_ERROR) {
      fprintf(stderr, "SELFTEST: %s framebuffer setup FAIL\n", name);
      goto cleanup;
    }
    if (draw_dmabuf_nv12_external(fd, buffer_size, frame_w, frame_h,
                                   frame_w, uv_offset, view_w, view_h, NULL) != 0) {
      /* A fresh negative cache entry identifies an import limitation. A valid
       * imported image followed by a GL error is an actual renderer failure. */
      if (dmabuf_nv12_external_count == 1 &&
          dmabuf_nv12_external[0].image == EGL_NO_IMAGE_KHR) {
        skip_reason = scaled ? "1920x2 CMA NV12 EGL import unavailable" :
                               "64x64 CMA NV12 EGL import unavailable";
        rc = 77;
      } else {
        fprintf(stderr, "SELFTEST: %s draw FAIL\n", name);
      }
      goto cleanup;
    }
    glReadPixels(0, 0, view_w, view_h, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    GLenum error = glGetError();
    if (error != GL_NO_ERROR) {
      fprintf(stderr, "SELFTEST: %s read pixels FAIL (GL error 0x%x)\n", name, error);
      goto cleanup;
    }

    if (quadrants) {
      for (unsigned int i = 0; i < sizeof(quadrant_probes) / sizeof(quadrant_probes[0]); i++) {
        int x = quadrant_probes[i][0], y = quadrant_probes[i][1];
        int q = 2 * (y < 32) + (x >= 32); /* glReadPixels starts at the bottom. */
        if (external_check_pixel(name, pixels, view_w, x, y, quadrant_rgb[q][0],
                                  quadrant_rgb[q][1], quadrant_rgb[q][2]) != 0)
          goto cleanup;
      }
    } else {
      for (int y = 0; y < view_h; y++) {
        for (int x = 0; x < view_w; x++) {
          int source_x = ((2 * x + 1) * 1920) / (2 * 1280);
          int level = (source_x & 1) ? 255 : 0;
          int r = test ? test->expect_r : level;
          int g = test ? test->expect_g : level;
          int b = test ? test->expect_b : level;
          if (external_check_pixel(name, pixels, view_w, x, y, r, g, b) != 0)
            goto cleanup;
        }
      }
    }
    printf("SELFTEST: %s OK\n", name);
  }
  rc = 0;

cleanup:
  if (cpu_writing)
    dmabuf_sync(fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
  dmabuf_nv12_external_reset();
  egl_set_color_mode(previous_use709, previous_full);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, previous_draw_fbo);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, previous_read_fbo);
  glBindRenderbuffer(GL_RENDERBUFFER, previous_rbo);
  glViewport(previous_viewport[0], previous_viewport[1],
             previous_viewport[2], previous_viewport[3]);
  if (fbo) glDeleteFramebuffers(1, &fbo);
  if (rbo) glDeleteRenderbuffers(1, &rbo);
  if (map != MAP_FAILED) munmap(map, buffer_size);
  if (fd >= 0) close(fd);
  if (acquired_context) {
    EGLBoolean restored;
    if (previous_display != EGL_NO_DISPLAY && previous_context != EGL_NO_CONTEXT)
      restored = eglMakeCurrent(previous_display, previous_draw_surface,
                                previous_read_surface, previous_context);
    else
      restored = eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    current = false;
    if (!restored) {
      fprintf(stderr, "SELFTEST: restore previous EGL binding FAIL (EGL error 0x%x)\n",
              eglGetError());
      if (rc == 0)
        rc = 1;
    }
  }
  if (rc == 77)
    printf("SELFTEST: NV12 external SKIP (%s)\n", skip_reason);
  else
    printf("SELFTEST: NV12 external %s\n", rc == 0 ? "PASSED" : "FAILED");
  return rc;
#endif
}

/* Zero-copy render for a *linear* NV12 capture buffer.  The optional external
 * path imports both planes as one NV12 image.  The default/fallback imports
 * luma as R8 and interleaved chroma as GR88, each with integer texelFetch.
 * uv_offset is a byte offset handed straight to EGL; uv_offset % pitch checks
 * that the chroma plane starts on a luma-row boundary. */
int egl_draw_dmabuf_nv12(int dmabuf_fd, unsigned int size, int frame_width,
                         int frame_height, int pitch, int uv_offset) {
  EGLImageKHR img_y, img_uv;
  EGLint surf_w = 0, surf_h = 0;

  if (zc_breakdown < 0)
    zc_breakdown = getenv("MOONLIGHT_ZC_BREAKDOWN") != NULL;
  double bd0 = zc_breakdown ? zc_now_ms() : 0.0;

  if ((!nv12_program && !nv12_external_available) || pitch <= 0 || pitch < frame_width ||
      uv_offset % pitch != 0)
    return -1;

  if (!current) {
    eglMakeCurrent(display, surface, surface, context);
    current = true;
  }

  if (frame_width != dmabuf_last_w || frame_height != dmabuf_last_h) {
    dmabuf_nv12_reset();
    dmabuf_nv12_external_reset();
    dmabuf_last_w = frame_width;
    dmabuf_last_h = frame_height;
  }

  eglQuerySurface(display, surface, EGL_WIDTH, &surf_w);
  eglQuerySurface(display, surface, EGL_HEIGHT, &surf_h);
  if (surf_w <= 0) surf_w = width;
  if (surf_h <= 0) surf_h = height;

  double bd1 = 0.0;
  if (nv12_external_requested) {
    if (draw_dmabuf_nv12_external(dmabuf_fd, size, frame_width, frame_height,
                                  pitch, uv_offset, surf_w, surf_h,
                                  zc_breakdown ? &bd1 : NULL) == 0) {
      if (!nv12_external_active_shown) {
        fprintf(stderr, "EGL: nv12 external path active fd=%d frame=%dx%d pitch=%d uv_off=%d\n",
                dmabuf_fd, frame_width, frame_height, pitch, uv_offset);
        nv12_external_active_shown = 1;
      }
      if (zc_breakdown) glFinish();
      double bd2 = zc_breakdown ? zc_now_ms() : 0.0;
      eglSwapBuffers(display, surface);
      double bd3 = zc_breakdown ? zc_now_ms() : 0.0;
      zc_record_breakdown(bd0, bd1, bd2, bd3);
      return 0;
    }
    if (nv12_external_draw_failures < 5) {
      nv12_external_draw_failures++;
      fprintf(stderr, "EGL: nv12 external draw failed; falling back to two-plane path\n");
    }
    if (!nv12_program)
      return -1;
  }

  if (dmabuf_get_nv12(dmabuf_fd, frame_height, pitch, uv_offset,
                      &img_y, &img_uv) != 0)
    return -1;
  bd1 = zc_breakdown ? zc_now_ms() : 0.0;

  glUseProgram(nv12_program);

  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, nv12_texture[0]);
  p_glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, img_y);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, nv12_texture[1]);
  p_glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, img_uv);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  glUniform1i(nv12_uniforms[0], 0);
  glUniform1i(nv12_uniforms[1], 1);
  glUniform1i(nv12_uniforms[2], frame_width);
  glUniform1i(nv12_uniforms[3], frame_height);
  glUniform1i(nv12_uniforms[4], surf_w);
  glUniform1i(nv12_uniforms[5], surf_h);
  glUniform1i(nv12_trivial_uniform, dmabuf_trivial);
  upload_color_params(nv12_color_uniforms);

  {
    static int dbg_shown;
    if (!dbg_shown) {
      fprintf(stderr, "EGL: nv12 two-plane import fd=%d size=%u pitch=%d "
                      "uv_off=%d frame=%dx%d view=%dx%d\n",
              dmabuf_fd, size, pitch, uv_offset, frame_width, frame_height,
              surf_w, surf_h);
      dbg_shown = 1;
    }
  }

  glDisable(GL_BLEND);
  glDisableVertexAttribArray(0);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  glActiveTexture(GL_TEXTURE0);

  if (zc_breakdown) glFinish();
  double bd2 = zc_breakdown ? zc_now_ms() : 0.0;

  eglSwapBuffers(display, surface);

  double bd3 = zc_breakdown ? zc_now_ms() : 0.0;
  zc_record_breakdown(bd0, bd1, bd2, bd3);
  return 0;
}

void egl_destroy() {
  if (!current) {
    eglMakeCurrent(display, surface, surface, context);
    current = true;
  }
  for (int i = 0; i < dmabuf_images_count; i++)
    if (p_eglDestroyImageKHR)
      p_eglDestroyImageKHR(display, dmabuf_images[i].image);
  dmabuf_images_count = 0;
  dmabuf_nv12_reset();
  dmabuf_nv12_external_reset();
  if (nv12_external_texture) {
    glDeleteTextures(1, &nv12_external_texture);
    nv12_external_texture = 0;
  }
  if (nv12_external_program) {
    glDeleteProgram(nv12_external_program);
    nv12_external_program = 0;
  }
  nv12_external_available = 0;
  nv12_external_requested = 0;
  glDeleteTextures(2, nv12_texture);
  nv12_texture[0] = 0;
  nv12_texture[1] = 0;
  eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  current = false;
  eglDestroySurface(display, surface);
  eglDestroyContext(display, context);
  eglTerminate(display);
}
