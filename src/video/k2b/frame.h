#ifndef K2B_FRAME_H
#define K2B_FRAME_H

#include <stddef.h>
#include <stdint.h>

enum k2b_pixel_format { K2B_PIXEL_NV12 = 1 };
enum k2b_matrix { K2B_MATRIX_BT601 = 1, K2B_MATRIX_BT709 = 2 };
enum k2b_range { K2B_RANGE_LIMITED = 1, K2B_RANGE_FULL = 2 };

struct k2b_frame {
  int fd;
  enum k2b_pixel_format format;
  size_t allocation_bytes;
  size_t y_offset;
  size_t uv_offset;
  uint32_t stride;
  uint32_t storage_height;
  uint32_t crop_x;
  uint32_t crop_y;
  uint32_t width;
  uint32_t height;
  enum k2b_matrix matrix;
  enum k2b_range range;
};

/* Metadata only: returns 0 if valid, -1 otherwise. No ownership transfer or
 * pixel reads; the descriptor's fd is not inspected or closed. */
int k2b_frame_validate(const struct k2b_frame *frame);

#endif
