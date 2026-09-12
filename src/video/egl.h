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

#include <EGL/egl.h>

void egl_init(EGLNativeDisplayType native_display, NativeWindowType native_window, int display_width, int display_height);
void egl_draw(uint8_t* image[3]);
int egl_draw_dmabuf(int dmabuf_fd, unsigned int size, int frame_width, int frame_height,
                    int uv_offset, int byte_pitch);
int egl_draw_dmabuf_nv12(int dmabuf_fd, unsigned int size, int frame_width, int frame_height,
                         int pitch, int uv_offset);
/* Set the YCbCr -> RGB conversion (limited/full range, 601/709 coefficients)
 * from the stream's own metadata. */
void egl_set_color_params(float yscale, float yoff, float rv, float gu, float gv, float bu);
void egl_destroy();
