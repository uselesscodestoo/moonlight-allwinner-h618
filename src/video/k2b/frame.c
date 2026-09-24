#include "frame.h"

int k2b_frame_validate(const struct k2b_frame *frame)
{
  size_t y_bytes;
  size_t uv_bytes;

  if (frame == NULL || frame->fd < 0 || frame->format != K2B_PIXEL_NV12 ||
      frame->y_offset != 0 || frame->width != 1920 || frame->height != 1080)
    return -1;

  if (frame->stride == 0 || frame->stride > 8192 ||
      frame->storage_height == 0 || frame->storage_height > 8192 ||
      (frame->stride & 1) || (frame->storage_height & 1) ||
      (frame->crop_x & 1) || (frame->crop_y & 1))
    return -1;

  if (frame->width > frame->stride || frame->height > frame->storage_height ||
      frame->crop_x > frame->stride - frame->width ||
      frame->crop_y > frame->storage_height - frame->height)
    return -1;

  if ((frame->matrix != K2B_MATRIX_BT601 && frame->matrix != K2B_MATRIX_BT709) ||
      (frame->range != K2B_RANGE_LIMITED && frame->range != K2B_RANGE_FULL))
    return -1;

  /* The 8192 dimension caps also keep this arithmetic safe on 32-bit size_t. */
  y_bytes = (size_t)frame->stride * frame->storage_height;
  uv_bytes = y_bytes / 2;
  return frame->uv_offset == y_bytes &&
         frame->allocation_bytes >= y_bytes + uv_bytes ? 0 : -1;
}
