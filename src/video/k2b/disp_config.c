#include "disp_config.h"

#include <string.h>

int k2b_disp_config_prepare(struct disp_layer_config2 *out,
                            const struct k2b_frame *frame, uint32_t frame_id)
{
  struct disp_fb_info2 *fb;

  if (out == NULL || k2b_frame_validate(frame) != 0)
    return -1;

  memset(out, 0, sizeof(*out));
  out->enable = true;
  out->channel = 0;
  out->layer_id = 0;
  out->info.mode = LAYER_MODE_BUFFER;
  out->info.zorder = 31;
  out->info.alpha_mode = 1;
  out->info.alpha_value = 255;
  out->info.screen_win.width = 1920;
  out->info.screen_win.height = 1080;
  out->info.id = frame_id;
  out->info.atw.cof_fd = -1;

  fb = &out->info.fb;
  fb->fd = frame->fd;
  fb->trd_right_fd = -1;
  fb->metadata_fd = -1;

  /* In 8-bit NV12, a Y sample is one byte and a UV pair is two bytes.
   * Vendor sizes are in samples/pixels, so stride bytes means stride Y
   * samples and stride/2 UV pairs. A 1088-row allocation has 544 UV rows;
   * its padded storage extent is separate from the 1080-row visible crop. */
  fb->size[0].width = frame->stride;
  fb->size[0].height = frame->storage_height;
  fb->size[1].width = frame->stride / 2;
  fb->size[1].height = frame->storage_height / 2;
  fb->format = DISP_FORMAT_YUV420_SP_UVUV;
  if (frame->matrix == K2B_MATRIX_BT601)
    fb->color_space = frame->range == K2B_RANGE_FULL ? DISP_BT601_F : DISP_BT601;
  else
    fb->color_space = frame->range == K2B_RANGE_FULL ? DISP_BT709_F : DISP_BT709;
  /* Match the existing SDR probe; the descriptor carries no HDR metadata. */
  fb->eotf = DISP_EOTF_GAMMA22;
  fb->flags = DISP_BF_NORMAL;
  fb->scan = DISP_SCAN_PROGRESSIVE;
  fb->crop.x = (int64_t)frame->crop_x << 32;
  fb->crop.y = (int64_t)frame->crop_y << 32;
  fb->crop.width = (int64_t)frame->width << 32;
  fb->crop.height = (int64_t)frame->height << 32;
  return 0;
}
