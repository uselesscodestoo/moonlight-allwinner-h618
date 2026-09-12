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

#include "video.h"
#include "egl.h"
#include "ffmpeg.h"
#ifdef HAVE_VAAPI
#include "ffmpeg_vaapi.h"
#endif

#include <libswscale/swscale.h>

#include "../input/x11.h"
#include "../loop.h"
#include "../util.h"

#include <X11/Xatom.h>
#include <X11/Xutil.h>
#ifdef HAVE_DPMS
#include <X11/extensions/dpms.h>
#endif
#ifdef HAVE_XSS
#include <X11/extensions/scrnsaver.h>
#endif

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <poll.h>

#define X11_VDPAU_ACCELERATION ENABLE_HARDWARE_ACCELERATION_1
#define X11_VAAPI_ACCELERATION ENABLE_HARDWARE_ACCELERATION_2
#define SLICES_PER_FRAME 4

static void* ffmpeg_buffer = NULL;
static size_t ffmpeg_buffer_size = 0;

static Display *display = NULL;
static Window window;

static int pipefd[2];

static int display_width;
static int display_height;

#ifdef HAVE_VAAPI
static AVFrame* vaapi_sw_nv12 = NULL;
static AVFrame* vaapi_sw_yuv420p = NULL;
static struct SwsContext* vaapi_sws = NULL;
static int vaapi_sws_w, vaapi_sws_h;

/* Temporary instrumentation: report which render path runs and the fps. */
static double dbg_t0;
static double dbg_last, dbg_cost_sum, dbg_cost_sum2, dbg_cost_sum3;
static volatile unsigned long dbg_submits, dbg_submits_shown, dbg_submit_window;
static int dbg_frames;
static char dbg_path_name[32];

static double now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}

static void dbg_note2(const char* label, int w, int h, double c1, double c2, double c3);

static void dbg_note(const char* label, int w, int h, double cost_ms) {
  dbg_note2(label, w, h, cost_ms, -1, -1);
}

static void dbg_note2(const char* label, int w, int h, double c1, double c2, double c3) {
  if (dbg_path_name[0] == '\0' || strcmp(dbg_path_name, label) != 0) {
    fprintf(stderr, "x11: render path -> %s\n", label);
    strncpy(dbg_path_name, label, sizeof(dbg_path_name) - 1);
    dbg_frames = 0;
    dbg_t0 = now_ms();
    dbg_last = dbg_t0;
    dbg_cost_sum = 0;
    dbg_cost_sum2 = 0;
    dbg_cost_sum3 = 0;
  }
  dbg_frames++;
  /* Decode units submitted (i.e. frames the host actually sent) in this
   * reporting window; compare with the rendered frame count. */
  dbg_submit_window += dbg_submits - dbg_submits_shown;
  dbg_submits_shown = dbg_submits;
  dbg_cost_sum += c1;
  dbg_cost_sum2 += c2;
  dbg_cost_sum3 += c3;
  if (dbg_frames % 10 == 0) {
    double now = now_ms();
    double dt = now - dbg_t0;
    double dfps = (now - dbg_last) > 0 ? 10000.0 / (now - dbg_last) : 0.0;
    fprintf(stderr, "x11: %s frame=%d %dx%d avg_fps=%.1f delta_fps=%.1f submits=%lu c1=%.1fms c2=%.1fms c3=%.1fms\n",
            label, dbg_frames, w, h, dt > 0 ? 1000.0 * dbg_frames / dt : 0.0,
            dfps, dbg_submit_window, dbg_cost_sum / 10.0, dbg_cost_sum2 / 10.0, dbg_cost_sum3 / 10.0);
    dbg_last = now;
    dbg_submit_window = 0;
    dbg_cost_sum = 0;
    dbg_cost_sum2 = 0;
    dbg_cost_sum3 = 0;
  }
}
#endif


/* A DPMS-off CRTC (blanked monitor) makes an EGL swap of a 1920x1080 buffer
 * cost ~750 ms instead of ~2 ms, which throttles streaming to ~1 fps, so keep
 * the output awake for as long as we are displaying. */
static void display_inhibit_blanking(Display* dpy, Bool inhibit) {
  int event_base, error_base;

  if (dpy == NULL)
    return;

  if (inhibit)
    XResetScreenSaver(dpy);

#ifdef HAVE_DPMS
  if (DPMSQueryExtension(dpy, &event_base, &error_base)) {
    if (inhibit)
      DPMSDisable(dpy);
    else
      DPMSEnable(dpy);
  }
#endif
#ifdef HAVE_XSS
  if (XScreenSaverQueryExtension(dpy, &event_base, &error_base))
    XScreenSaverSuspend(dpy, inhibit);
#else
  (void) event_base;
  (void) error_base;
#endif
  XFlush(dpy);
}

static int frame_handle(int pipefd) {
  AVFrame* frame = NULL;
  while (read(pipefd, &frame, sizeof(void*)) > 0);
  if (frame) {
    if (ffmpeg_decoder == SOFTWARE)
      egl_draw(frame->data);
    #ifdef HAVE_VAAPI
    else if (ffmpeg_decoder == VAAPI) {
      /* Debug switches: MOONLIGHT_NO_ZC=1 forces the download fallback,
       * MOONLIGHT_NO_DRAW=1 drops frames without rendering. */
      static int no_zc = -1, no_draw = -1;
      static int zc_tiled = -1;
      if (no_zc < 0) {
        no_zc = getenv("MOONLIGHT_NO_ZC") != NULL;
        no_draw = getenv("MOONLIGHT_NO_DRAW") != NULL;
        /* The tiled de-tile shader is correct but far too expensive on
         * Mali-G31 (measured ~200 ms/frame at 720p), so the download path is
         * the better fallback for tiled capture.  Opt in for experiments. */
        zc_tiled = getenv("MOONLIGHT_ZC_TILED") != NULL;
      }
      if (no_draw) {
        dbg_note("drop", frame->width, frame->height, 0);
        return LOOP_OK;
      }

      /* Zero-copy: hand the decoder's dma-buf straight to the GPU. */
      VADRMPRIMESurfaceDescriptor* desc = NULL;
      int zc_attempted = 0;
      if (!no_zc) {
        double t0 = now_ms();
        int exp_rc = vaapi_export_dmabuf(frame, &desc);
        double t1 = now_ms();
        if (exp_rc == 0 && desc != NULL &&
            desc->num_objects > 0 && desc->num_layers > 0) {
          int rc;
          zc_attempted = 1;
          if (desc->objects[0].drm_format_modifier == 0)
            /* Linear NV12 (V4L2_CAPTURE_FORMAT=nv12): import as native NV12. */
            rc = egl_draw_dmabuf_nv12(desc->objects[0].fd, desc->objects[0].size,
                                      frame->width, frame->height,
                                      desc->layers[0].pitch[0],
                                      desc->layers[0].offset[1]);
          else if (zc_tiled)
            /* Tiled NV12: import as R8 and de-tile in the shader. */
            rc = egl_draw_dmabuf(desc->objects[0].fd, desc->objects[0].size,
                                 frame->width, frame->height,
                                 desc->layers[0].offset[1],
                                 desc->layers[0].pitch[0]);
          else
            rc = -1;
          if (rc == 0) {
            dbg_note2("zero-copy", frame->width, frame->height, t1 - t0, now_ms() - t1, -1);
            return LOOP_OK;
          }
        }
        if (zc_attempted)
          dbg_note2("zero-copy-failed", frame->width, frame->height, t1 - t0, now_ms() - t1, -1);
      }

      /* Fallback: download to system memory and render as planar YUV. */
      if (vaapi_sw_nv12 == NULL || vaapi_sw_nv12->width != frame->width ||
          vaapi_sw_nv12->height != frame->height) {
        av_frame_free(&vaapi_sw_nv12);
        if (vaapi_sw_nv12 == NULL) {
          vaapi_sw_nv12 = av_frame_alloc();
          if (vaapi_sw_nv12 != NULL) {
            vaapi_sw_nv12->format = AV_PIX_FMT_NV12;
            vaapi_sw_nv12->width = frame->width;
            vaapi_sw_nv12->height = frame->height;
            av_frame_get_buffer(vaapi_sw_nv12, 32);
          }
        }
      }
      double f0 = now_ms();
      if (vaapi_sw_nv12 != NULL && vaapi_transfer(vaapi_sw_nv12, frame) == 0) {
        double f1 = now_ms();
        if (vaapi_sws == NULL || vaapi_sws_w != frame->width ||
            vaapi_sws_h != frame->height) {
          if (vaapi_sws != NULL)
            sws_freeContext(vaapi_sws);
          vaapi_sws = sws_getContext(frame->width, frame->height,
                                     AV_PIX_FMT_NV12,
                                     frame->width, frame->height,
                                     AV_PIX_FMT_YUV420P,
                                     SWS_BILINEAR, NULL, NULL, NULL);
          vaapi_sws_w = frame->width;
          vaapi_sws_h = frame->height;
        }
        if (vaapi_sws != NULL) {
          double f2 = now_ms();
          sws_scale(vaapi_sws, (const uint8_t* const*) vaapi_sw_nv12->data,
                    vaapi_sw_nv12->linesize, 0, frame->height,
                    vaapi_sw_yuv420p->data, vaapi_sw_yuv420p->linesize);
          double f3 = now_ms();
          egl_draw(vaapi_sw_yuv420p->data);
          double f4 = now_ms();
          dbg_note2("fallback-download", frame->width, frame->height,
                    f1 - f0, f3 - f2, f4 - f3);
        }
      }
    }
    #endif
  }

  return LOOP_OK;
}

int x11_init(bool vdpau, bool vaapi) {
  XInitThreads();
  display = XOpenDisplay(NULL);
  if (!display)
    return 0;

  #ifdef HAVE_VAAPI
  if (vaapi && vaapi_init_lib(display) == 0)
    return INIT_VAAPI;
  #endif

  return INIT_EGL;
}

int x11_setup(int videoFormat, int width, int height, int redrawRate, void* context, int drFlags) {
  ensure_buf_size(&ffmpeg_buffer, &ffmpeg_buffer_size, INITIAL_DECODER_BUFFER_SIZE + AV_INPUT_BUFFER_PADDING_SIZE);

  if (!display) {
    fprintf(stderr, "Error: failed to open X display.\n");
    return -1;
  }

  if (drFlags & DISPLAY_FULLSCREEN) {
    Screen* screen = DefaultScreenOfDisplay(display);
    display_width = WidthOfScreen(screen);
    display_height = HeightOfScreen(screen);
  } else {
    display_width = width;
    display_height = height;
  }

  Window root = DefaultRootWindow(display);
  display_inhibit_blanking(display, True);

  XSetWindowAttributes winattr = { .event_mask = PointerMotionMask | ButtonPressMask | ButtonReleaseMask | KeyPressMask | KeyReleaseMask };
  window = XCreateWindow(display, root, 0, 0, display_width, display_height, 0, CopyFromParent, InputOutput, CopyFromParent, CWEventMask, &winattr);
  XMapWindow(display, window);
  XStoreName(display, window, "Moonlight");

  if (drFlags & DISPLAY_FULLSCREEN) {
    Atom wm_state = XInternAtom(display, "_NET_WM_STATE", False);
    Atom fullscreen = XInternAtom(display, "_NET_WM_STATE_FULLSCREEN", False);

    XEvent xev = {0};
    xev.type = ClientMessage;
    xev.xclient.window = window;
    xev.xclient.message_type = wm_state;
    xev.xclient.format = 32;
    xev.xclient.data.l[0] = 1;
    xev.xclient.data.l[1] = fullscreen;
    xev.xclient.data.l[2] = 0;

    XSendEvent(display, DefaultRootWindow(display), False, SubstructureRedirectMask | SubstructureNotifyMask, &xev);
  }
  XFlush(display);

  int avc_flags;
  if (drFlags & X11_VDPAU_ACCELERATION)
    avc_flags = VDPAU_ACCELERATION;
  else if (drFlags & X11_VAAPI_ACCELERATION)
    avc_flags = VAAPI_ACCELERATION;
  else
    avc_flags = SLICE_THREADING;

  if (ffmpeg_init(videoFormat, width, height, avc_flags, 2, SLICES_PER_FRAME) < 0) {
    fprintf(stderr, "Couldn't initialize video decoding\n");
    return -1;
  }

  egl_init(display, window, width, height);

  #ifdef HAVE_VAAPI
  if (ffmpeg_decoder == VAAPI) {
    vaapi_sw_nv12 = av_frame_alloc();
    vaapi_sw_nv12->format = AV_PIX_FMT_NV12;
    vaapi_sw_nv12->width = width;
    vaapi_sw_nv12->height = height;
    if (av_frame_get_buffer(vaapi_sw_nv12, 32) < 0) {
      fprintf(stderr, "Couldn't allocate VAAPI download frame\n");
      return -1;
    }

    vaapi_sw_yuv420p = av_frame_alloc();
    vaapi_sw_yuv420p->format = AV_PIX_FMT_YUV420P;
    vaapi_sw_yuv420p->width = width;
    vaapi_sw_yuv420p->height = height;
    if (av_frame_get_buffer(vaapi_sw_yuv420p, 32) < 0) {
      fprintf(stderr, "Couldn't allocate VAAPI convert frame\n");
      return -1;
    }

    vaapi_sws = sws_getContext(width, height, AV_PIX_FMT_NV12,
                               width, height, AV_PIX_FMT_YUV420P,
                               SWS_BILINEAR, NULL, NULL, NULL);
    if (vaapi_sws == NULL) {
      fprintf(stderr, "Couldn't create conversion context\n");
      return -1;
    }
  }
  #endif

  if (pipe(pipefd) == -1) {
    fprintf(stderr, "Can't create communication channel between threads\n");
    return -2;
  }
  loop_add_fd(pipefd[0], &frame_handle, POLLIN);
  fcntl(pipefd[0], F_SETFL, O_NONBLOCK);

  x11_input_init(display, window);

  return 0;
}

int x11_setup_vdpau(int videoFormat, int width, int height, int redrawRate, void* context, int drFlags) {
  return x11_setup(videoFormat, width, height, redrawRate, context, drFlags | X11_VDPAU_ACCELERATION);
}

int x11_setup_vaapi(int videoFormat, int width, int height, int redrawRate, void* context, int drFlags) {
  return x11_setup(videoFormat, width, height, redrawRate, context, drFlags | X11_VAAPI_ACCELERATION);
}

void x11_cleanup() {
  display_inhibit_blanking(display, False);

  ffmpeg_destroy();
  egl_destroy();
  #ifdef HAVE_VAAPI
  if (vaapi_sw_yuv420p != NULL)
    av_frame_free(&vaapi_sw_yuv420p);
  if (vaapi_sw_nv12 != NULL)
    av_frame_free(&vaapi_sw_nv12);
  if (vaapi_sws != NULL)
    sws_freeContext(vaapi_sws);
  #endif
}

int x11_submit_decode_unit(PDECODE_UNIT decodeUnit) {
  PLENTRY entry = decodeUnit->bufferList;
  int length = 0;

  ensure_buf_size(&ffmpeg_buffer, &ffmpeg_buffer_size, decodeUnit->fullLength + AV_INPUT_BUFFER_PADDING_SIZE);

  while (entry != NULL) {
    memcpy(ffmpeg_buffer+length, entry->data, entry->length);
    length += entry->length;
    entry = entry->next;
  }

  dbg_submits++;
  ffmpeg_decode(ffmpeg_buffer, length);

  AVFrame* frame = ffmpeg_get_frame(true);
  if (frame != NULL)
    write(pipefd[1], &frame, sizeof(void*));

  return DR_OK;
}

DECODER_RENDERER_CALLBACKS decoder_callbacks_x11 = {
  .setup = x11_setup,
  .cleanup = x11_cleanup,
  .submitDecodeUnit = x11_submit_decode_unit,
  .capabilities = CAPABILITY_SLICES_PER_FRAME(SLICES_PER_FRAME) | CAPABILITY_REFERENCE_FRAME_INVALIDATION_HEVC | CAPABILITY_DIRECT_SUBMIT,
};

DECODER_RENDERER_CALLBACKS decoder_callbacks_x11_vdpau = {
  .setup = x11_setup_vdpau,
  .cleanup = x11_cleanup,
  .submitDecodeUnit = x11_submit_decode_unit,
  .capabilities = CAPABILITY_DIRECT_SUBMIT,
};

DECODER_RENDERER_CALLBACKS decoder_callbacks_x11_vaapi = {
  .setup = x11_setup_vaapi,
  .cleanup = x11_cleanup,
  .submitDecodeUnit = x11_submit_decode_unit,
  .capabilities = CAPABILITY_DIRECT_SUBMIT,
};
