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

#include <va/va.h>
#include <va/va_x11.h>
#include <va/va_drmcommon.h>
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vaapi.h>
#include <X11/Xlib.h>

#include <string.h>
#include <unistd.h>

#define MAX_SURFACES 6

static AVBufferRef* device_ref;

static enum AVPixelFormat va_get_format(AVCodecContext* context, const enum AVPixelFormat* pixel_format) {
  AVBufferRef* hw_ctx = av_hwframe_ctx_alloc(device_ref);
  if (hw_ctx == NULL) {
    fprintf(stderr, "Failed to initialize Vaapi buffer\n");
    return AV_PIX_FMT_NONE;
  }

  AVHWFramesContext* fr_ctx = (AVHWFramesContext*) hw_ctx->data;
  fr_ctx->format = AV_PIX_FMT_VAAPI;
  fr_ctx->sw_format = AV_PIX_FMT_NV12;
  fr_ctx->width = context->coded_width;
  fr_ctx->height = context->coded_height;
  fr_ctx->initial_pool_size = MAX_SURFACES + 1;

  if (av_hwframe_ctx_init(hw_ctx) < 0) {
    fprintf(stderr, "Failed to initialize VAAPI frame context");
    return AV_PIX_FMT_NONE;
  }

  context->pix_fmt = AV_PIX_FMT_VAAPI;
  context->hw_device_ctx = device_ref;
  context->hw_frames_ctx = hw_ctx;
  context->slice_flags = SLICE_FLAG_CODED_ORDER | SLICE_FLAG_ALLOW_FIELD;
  return AV_PIX_FMT_VAAPI;
}

static int va_get_buffer(AVCodecContext* context, AVFrame* frame, int flags) {
  return av_hwframe_get_buffer(context->hw_frames_ctx, frame, 0);
}

int vaapi_init_lib() {
  return av_hwdevice_ctx_create(&device_ref, AV_HWDEVICE_TYPE_VAAPI, ":0", NULL, 0);
}

int vaapi_init(AVCodecContext* decoder_ctx) {
  decoder_ctx->get_format = va_get_format;
  decoder_ctx->get_buffer2 = va_get_buffer;
  return 0;
}

int vaapi_transfer(AVFrame* dst, AVFrame* src) {
  return av_hwframe_transfer_data(dst, src, 0);
}

#define VAAPI_MAX_EXPORTS 64

static struct {
  VASurfaceID surface;
  VADRMPRIMESurfaceDescriptor desc;
} vaapi_exports[VAAPI_MAX_EXPORTS];
static int vaapi_exports_count;

void vaapi_export_reset(void) {
  for (int i = 0; i < vaapi_exports_count; i++)
    for (unsigned int j = 0; j < vaapi_exports[i].desc.num_objects; j++)
      if (vaapi_exports[i].desc.objects[j].fd >= 0)
        close(vaapi_exports[i].desc.objects[j].fd);
  vaapi_exports_count = 0;
}

int vaapi_export_dmabuf(AVFrame* dec_frame, VADRMPRIMESurfaceDescriptor** desc) {
  VASurfaceID surface = (VASurfaceID)(uintptr_t)dec_frame->data[3];
  AVHWDeviceContext* device = (AVHWDeviceContext*) device_ref->data;
  AVVAAPIDeviceContext* va_ctx = device->hwctx;
  VAStatus st;
  int i;

  for (i = 0; i < vaapi_exports_count; i++) {
    if (vaapi_exports[i].surface == surface) {
      *desc = &vaapi_exports[i].desc;
      return 0;
    }
  }

  if (vaapi_exports_count >= VAAPI_MAX_EXPORTS) {
    fprintf(stderr, "vaapi_export_dmabuf: export cache full\n");
    return -1;
  }

  memset(&vaapi_exports[vaapi_exports_count].desc, 0,
         sizeof(vaapi_exports[vaapi_exports_count].desc));

  st = vaExportSurfaceHandle(va_ctx->display, surface,
        VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
        VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_SEPARATE_LAYERS,
        &vaapi_exports[vaapi_exports_count].desc);
  if (st != VA_STATUS_SUCCESS) {
    fprintf(stderr, "vaExportSurfaceHandle failed: %s\n", vaErrorStr(st));
    return -1;
  }

  vaapi_exports[vaapi_exports_count].surface = surface;
  *desc = &vaapi_exports[vaapi_exports_count].desc;
  vaapi_exports_count++;
  return 0;
}

void vaapi_queue(AVFrame* dec_frame, Window win, int width, int height) {
  VASurfaceID surface = (VASurfaceID)(uintptr_t)dec_frame->data[3];
  AVHWDeviceContext* device = (AVHWDeviceContext*) device_ref->data;
  AVVAAPIDeviceContext *va_ctx = device->hwctx;
  vaPutSurface(va_ctx->display, surface, win, 0, 0, dec_frame->width, dec_frame->height, 0, 0, width, height, NULL, 0, 0);
}
