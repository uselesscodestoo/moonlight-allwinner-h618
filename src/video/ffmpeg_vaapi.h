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
#include <va/va_drmcommon.h>
#include <X11/Xlib.h>

int vaapi_init_lib();
int vaapi_init(AVCodecContext* decoder_ctx);
void vaapi_queue(AVFrame* dec_frame, Window win, int width, int height);
int vaapi_transfer(AVFrame* dst, AVFrame* src);

/* Export a decoded VAAPI frame's backing buffer as a DRM PRIME dma-buf.
 * Returns 0 on success and stores a cached descriptor in *desc.  The fds in
 * the descriptor stay owned by this module. */
int vaapi_export_dmabuf(AVFrame* dec_frame, VADRMPRIMESurfaceDescriptor** desc);
void vaapi_export_reset(void);
