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
#include <string.h>
#include <pthread.h>
#include <unistd.h>

#ifndef DRM_FORMAT_R8
#define DRM_FORMAT_R8 0x20203852 /* 'R', '8', ' ', ' ' */
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
in mediump vec2 tex_position;\n\
out lowp vec4 fragColor;\n\
void main() {\n\
  mediump float y = texture(ymap, tex_position).r;\n\
  mediump float u = texture(umap, tex_position).r - .5;\n\
  mediump float v = texture(vmap, tex_position).r - .5;\n\
  lowp float r = y + 1.28033 * v;\n\
  lowp float g = y - .21482 * u - .38059 * v;\n\
  lowp float b = y + 2.12798 * u;\n\
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
  float Yp = (Y - 16.0 / 255.0) * 1.164383;\n\
  float Up = U - 128.0 / 255.0;\n\
  float Vp = V - 128.0 / 255.0;\n\
  vec3 rgb = vec3(Yp + 1.792741 * Vp,\n\
                  Yp - 0.213249 * Up - 0.532909 * Vp,\n\
                  Yp + 2.112402 * Up);\n\
  outColor = vec4(clamp(rgb, 0.0, 1.0), 1.0);\n\
}\n";

/* Linear NV12 path: the capture dma-buf is a plain NV12 buffer, so the
 * whole buffer is imported once as an R8 image (stride = luma pitch) and
 * both planes are fetched with texelFetch.  Chroma plane row offset is
 * uv_offset / pitch.  No de-tiling math is needed. */
static const char* nv12_fragment_source = "\
#version 300 es\n\
precision highp float;\n\
precision highp int;\n\
precision highp sampler2D;\n\
uniform sampler2D u_tex;\n\
uniform int u_uv_row;\n\
uniform int u_frame_w;\n\
uniform int u_frame_h;\n\
uniform int u_view_w;\n\
uniform int u_view_h;\n\
uniform int u_trivial;\n\
layout(location = 0) out vec4 outColor;\n\
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
  float Y = texelFetch(u_tex, ivec2(x, y), 0).r;\n\
  float U = texelFetch(u_tex, ivec2(2 * cx, u_uv_row + cy), 0).r;\n\
  float V = texelFetch(u_tex, ivec2(2 * cx + 1, u_uv_row + cy), 0).r;\n\
  float Yp = (Y - 16.0 / 255.0) * 1.164383;\n\
  float Up = U - 128.0 / 255.0;\n\
  float Vp = V - 128.0 / 255.0;\n\
  vec3 rgb = vec3(Yp + 1.792741 * Vp,\n\
                  Yp - 0.213249 * Up - 0.532909 * Vp,\n\
                  Yp + 2.112402 * Up);\n\
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
static GLuint dmabuf_texture;
static GLint dmabuf_uniforms[9];
static int dmabuf_trivial;
static struct {
  int fd;
  EGLImageKHR image;
} dmabuf_images[DMABUF_MAX_IMAGES];
static int dmabuf_images_count;
static int dmabuf_last_w, dmabuf_last_h;

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

  p_eglCreateImageKHR = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
  p_eglDestroyImageKHR = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
  p_glEGLImageTargetTexture2DOES = (moonlight_eglimage_target_texture2does)eglGetProcAddress("glEGLImageTargetTexture2DOES");

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
    dmabuf_uniforms[8] = glGetUniformLocation(dmabuf_program, "u_trivial");
    dmabuf_trivial = getenv("MOONLIGHT_ZC_TRIVIAL") != NULL;
    glGenTextures(1, &dmabuf_texture);

    nv12_program = compile_program(dmabuf_vertex_source, nv12_fragment_source, "nv12-linear");
    nv12_uniforms[0] = glGetUniformLocation(nv12_program, "u_tex");
    nv12_uniforms[1] = glGetUniformLocation(nv12_program, "u_uv_row");
    nv12_uniforms[2] = glGetUniformLocation(nv12_program, "u_frame_w");
    nv12_uniforms[3] = glGetUniformLocation(nv12_program, "u_frame_h");
    nv12_uniforms[4] = glGetUniformLocation(nv12_program, "u_view_w");
    nv12_uniforms[5] = glGetUniformLocation(nv12_program, "u_view_h");
    nv12_trivial_uniform = glGetUniformLocation(nv12_program, "u_trivial");
  } else {
    fprintf(stderr, "EGL: dmabuf import extensions unavailable\n");
    dmabuf_program = 0;
  }

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

/* Zero-copy render for a *linear* NV12 capture buffer.  uv_offset must be a
 * whole number of luma rows (it is, because the luma plane is allocated
 * height-aligned), which lets a single R8 image cover both planes. */
int egl_draw_dmabuf_nv12(int dmabuf_fd, unsigned int size, int frame_width, int frame_height,
                         int pitch, int uv_offset) {
  EGLImageKHR img;
  int tex_height, uv_row;
  EGLint surf_w = 0, surf_h = 0;

  if (!nv12_program || pitch <= 0 || uv_offset % pitch != 0)
    return -1;
  uv_row = uv_offset / pitch;

  if (!current) {
    eglMakeCurrent(display, surface, surface, context);
    current = true;
  }

  if (frame_width != dmabuf_last_w || frame_height != dmabuf_last_h) {
    for (int i = 0; i < dmabuf_images_count; i++)
      if (p_eglDestroyImageKHR)
        p_eglDestroyImageKHR(display, dmabuf_images[i].image);
    dmabuf_images_count = 0;
    dmabuf_last_w = frame_width;
    dmabuf_last_h = frame_height;
  }

  tex_height = ((int)size + pitch - 1) / pitch;
  img = dmabuf_get_image(dmabuf_fd, pitch, tex_height);
  if (img == EGL_NO_IMAGE_KHR)
    return -1;

  eglQuerySurface(display, surface, EGL_WIDTH, &surf_w);
  eglQuerySurface(display, surface, EGL_HEIGHT, &surf_h);
  if (surf_w <= 0)
    surf_w = width;
  if (surf_h <= 0)
    surf_h = height;

  glUseProgram(nv12_program);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, dmabuf_texture);
  p_glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, img);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  glUniform1i(nv12_uniforms[0], 0);
  glUniform1i(nv12_uniforms[1], uv_row);
  glUniform1i(nv12_uniforms[2], frame_width);
  glUniform1i(nv12_uniforms[3], frame_height);
  glUniform1i(nv12_uniforms[4], surf_w);
  glUniform1i(nv12_uniforms[5], surf_h);
  glUniform1i(nv12_trivial_uniform, dmabuf_trivial);

  {
    static int dbg_shown;
    if (!dbg_shown) {
      fprintf(stderr, "EGL: nv12 dmabuf import fd=%d size=%u pitch=%d tex_h=%d uv_row=%d "
                      "frame=%dx%d view=%dx%d\n",
              dmabuf_fd, size, pitch, tex_height, uv_row, frame_width, frame_height, surf_w, surf_h);
      dbg_shown = 1;
    }
  }

  glDisable(GL_BLEND);
  glDisableVertexAttribArray(0);
  glDrawArrays(GL_TRIANGLES, 0, 3);

  eglSwapBuffers(display, surface);
  return 0;
}

void egl_destroy() {
  for (int i = 0; i < dmabuf_images_count; i++)
    if (p_eglDestroyImageKHR)
      p_eglDestroyImageKHR(display, dmabuf_images[i].image);
  dmabuf_images_count = 0;
  eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  eglDestroySurface(display, surface);
  eglDestroyContext(display, context);
  eglTerminate(display);
}
