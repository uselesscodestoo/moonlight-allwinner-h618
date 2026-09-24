#include "cedar_picture.h"

#include <errno.h>

int k2b_cedar_picture_frame(const VideoPicture *picture,
                           const struct k2b_cedar_buffer *y,
                           const struct k2b_cedar_buffer *uv,
                           enum k2b_matrix matrix, enum k2b_range range,
                           struct k2b_frame *out)
{
  struct k2b_frame frame = {0};
  uintptr_t base;
  size_t used;

  if (picture == NULL || y == NULL || uv == NULL || out == NULL)
    goto invalid;

  if (picture->ePixelFormat != PIXEL_FORMAT_NV12 ||
      picture->nStreamIndex != 0 || picture->bIsProgressive != 1 ||
      picture->b10BitPicFlag || picture->bEnableAfbcFlag ||
      picture->bFrameErrorFlag || picture->bTopFieldError ||
      picture->bBottomFieldError || picture->pData2 != NULL ||
      picture->pData3 != NULL)
    goto invalid;

  if (picture->nWidth <= 0 || picture->nWidth > 8192 ||
      picture->nHeight <= 0 || picture->nHeight > 8192 ||
      picture->nLineStride <= 0 || picture->nLineStride > 8192 ||
      picture->nWidth > picture->nLineStride ||
      picture->nLeftOffset < 0 || picture->nTopOffset < 0 ||
      picture->nRightOffset < picture->nLeftOffset ||
      picture->nBottomOffset < picture->nTopOffset ||
      picture->nRightOffset > picture->nWidth ||
      picture->nBottomOffset > picture->nHeight)
    goto invalid;

  if (y->base == NULL || uv->base == NULL || y->base != uv->base ||
      y->fd < 0 || uv->fd < 0 || y->fd != uv->fd ||
      y->bytes != uv->bytes || y->ve_address != uv->ve_address ||
      y->offset != 0 || uv->offset >= y->bytes)
    goto invalid;

  /* Integer addresses avoid subtraction of unrelated pointers. Check the
   * entire allocation range before adding the described UV offset. */
  base = (uintptr_t)y->base;
  if (y->bytes > UINTPTR_MAX - base ||
      (uintptr_t)picture->pData0 != base ||
      (uintptr_t)picture->pData1 != base + uv->offset)
    goto invalid;

  frame.fd = y->fd;
  frame.format = K2B_PIXEL_NV12;
  frame.allocation_bytes = y->bytes;
  frame.y_offset = y->offset;
  frame.uv_offset = uv->offset;
  frame.stride = (uint32_t)picture->nLineStride;
  frame.storage_height = (uint32_t)picture->nHeight;
  frame.crop_x = (uint32_t)picture->nLeftOffset;
  frame.crop_y = (uint32_t)picture->nTopOffset;
  frame.width = (uint32_t)(picture->nRightOffset - picture->nLeftOffset);
  frame.height = (uint32_t)(picture->nBottomOffset - picture->nTopOffset);
  frame.matrix = matrix;
  frame.range = range;
  if (k2b_frame_validate(&frame) != 0)
    goto invalid;

  /* Frame validation bounds the plane sizes before this addition. */
  used = frame.uv_offset + frame.uv_offset / 2;
  if (picture->nBufSize < 0 || (size_t)picture->nBufSize < used ||
      (size_t)picture->nBufSize > frame.allocation_bytes)
    goto invalid;

  *out = frame;
  return 0;

invalid:
  errno = EINVAL;
  return -1;
}
