#include "frame.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

static unsigned int checks;
static unsigned int failures;

static struct k2b_frame valid_frame(void)
{
  struct k2b_frame frame = {
    .fd = 0,
    .format = K2B_PIXEL_NV12,
    .allocation_bytes = (size_t)1920 * 1088 * 3 / 2,
    .y_offset = 0,
    .uv_offset = (size_t)1920 * 1088,
    .stride = 1920,
    .storage_height = 1088,
    .crop_x = 0,
    .crop_y = 0,
    .width = 1920,
    .height = 1080,
    .matrix = K2B_MATRIX_BT709,
    .range = K2B_RANGE_LIMITED
  };
  return frame;
}

static void check_frame(const char *name, const struct k2b_frame *frame,
                        int expected)
{
  int actual = k2b_frame_validate(frame);
  checks++;
  if (actual != expected) {
    fprintf(stderr, "FAIL: %s: expected %d, got %d\n", name, expected, actual);
    failures++;
  }
}

static void set_layout(struct k2b_frame *frame, uint32_t stride,
                       uint32_t storage_height)
{
  frame->stride = stride;
  frame->storage_height = storage_height;
  frame->uv_offset = (size_t)stride * storage_height;
  frame->allocation_bytes = frame->uv_offset + frame->uv_offset / 2;
}

static void test_valid_frames(void)
{
  struct k2b_frame frame = valid_frame();
  enum k2b_matrix matrices[] = { K2B_MATRIX_BT601, K2B_MATRIX_BT709 };
  enum k2b_range ranges[] = { K2B_RANGE_LIMITED, K2B_RANGE_FULL };
  size_t i;
  size_t j;

  check_frame("baseline NV12 descriptor with fd zero", &frame, 0);
  frame.fd = INT_MAX;
  check_frame("fd is metadata only", &frame, 0);

  frame = valid_frame();
  set_layout(&frame, 1920, 1080);
  check_frame("compact storage height", &frame, 0);

  frame = valid_frame();
  set_layout(&frame, 2048, 1088);
  check_frame("padded stride", &frame, 0);
  frame.crop_x = 128;
  frame.crop_y = 8;
  check_frame("even crop reaches right and bottom bounds", &frame, 0);

  frame = valid_frame();
  set_layout(&frame, 8192, 8192);
  frame.crop_x = 8192 - 1920;
  frame.crop_y = 8192 - 1080;
  check_frame("maximum supported storage dimensions", &frame, 0);

  frame = valid_frame();
  frame.allocation_bytes++;
  check_frame("allocation may contain trailing padding", &frame, 0);
  frame.allocation_bytes = SIZE_MAX;
  check_frame("large allocation does not overflow validation", &frame, 0);

  frame = valid_frame();
  for (i = 0; i < sizeof(matrices) / sizeof(matrices[0]); i++) {
    for (j = 0; j < sizeof(ranges) / sizeof(ranges[0]); j++) {
      frame.matrix = matrices[i];
      frame.range = ranges[j];
      check_frame("supported matrix and range combination", &frame, 0);
    }
  }
}

#define CHECK_INVALID(name, field, value) do { \
  struct k2b_frame changed = valid_frame(); \
  changed.field = (value); \
  check_frame((name), &changed, -1); \
} while (0)

static void test_invalid_frames(void)
{
  struct k2b_frame frame;

  check_frame("null descriptor", NULL, -1);
  CHECK_INVALID("negative fd", fd, -1);
  CHECK_INVALID("zero format", format, 0);
  CHECK_INVALID("unknown format", format, 2);
  CHECK_INVALID("negative format", format, -1);
  CHECK_INVALID("nonzero Y offset", y_offset, 2);
  CHECK_INVALID("huge Y offset", y_offset, SIZE_MAX);
  CHECK_INVALID("zero UV offset", uv_offset, 0);
  CHECK_INVALID("UV offset before Y end", uv_offset, (size_t)1920 * 1088 - 1);
  CHECK_INVALID("UV offset after Y end", uv_offset, (size_t)1920 * 1088 + 1);
  CHECK_INVALID("huge UV offset", uv_offset, SIZE_MAX);
  CHECK_INVALID("zero allocation", allocation_bytes, 0);
  CHECK_INVALID("allocation one byte short", allocation_bytes,
                (size_t)1920 * 1088 * 3 / 2 - 1);
  CHECK_INVALID("allocation contains only Y plane", allocation_bytes,
                (size_t)1920 * 1088);
  CHECK_INVALID("odd vertical crop", crop_y, 1);
  CHECK_INVALID("horizontal crop beyond right bound", crop_x, 2);
  CHECK_INVALID("vertical crop beyond bottom bound", crop_y, 10);
  CHECK_INVALID("huge odd horizontal crop", crop_x, UINT32_MAX);
  CHECK_INVALID("huge odd vertical crop", crop_y, UINT32_MAX);
  CHECK_INVALID("huge even horizontal crop", crop_x, UINT32_MAX - 1);
  CHECK_INVALID("huge even vertical crop", crop_y, UINT32_MAX - 1);
  CHECK_INVALID("zero visible width", width, 0);
  CHECK_INVALID("smaller visible width", width, 1918);
  CHECK_INVALID("larger visible width", width, 1922);
  CHECK_INVALID("huge visible width", width, UINT32_MAX);
  CHECK_INVALID("zero visible height", height, 0);
  CHECK_INVALID("smaller visible height", height, 1078);
  CHECK_INVALID("larger visible height", height, 1082);
  CHECK_INVALID("huge visible height", height, UINT32_MAX);
  CHECK_INVALID("zero matrix", matrix, 0);
  CHECK_INVALID("unknown matrix", matrix, 3);
  CHECK_INVALID("negative matrix", matrix, -1);
  CHECK_INVALID("zero range", range, 0);
  CHECK_INVALID("unknown range", range, 3);
  CHECK_INVALID("negative range", range, -1);
  CHECK_INVALID("huge stride", stride, UINT32_MAX);
  CHECK_INVALID("huge storage height", storage_height, UINT32_MAX);

  /* Keep plane metadata consistent so these check the storage constraints. */
  frame = valid_frame();
  set_layout(&frame, 0, 1088);
  check_frame("zero stride", &frame, -1);
  set_layout(&frame, 1920, 0);
  check_frame("zero storage height", &frame, -1);
  set_layout(&frame, 1921, 1088);
  check_frame("odd stride", &frame, -1);
  set_layout(&frame, 1920, 1089);
  check_frame("odd storage height", &frame, -1);
  set_layout(&frame, 1918, 1088);
  check_frame("stride below visible width", &frame, -1);
  set_layout(&frame, 1920, 1078);
  check_frame("storage below visible height", &frame, -1);
  set_layout(&frame, 8194, 1088);
  check_frame("stride above cap", &frame, -1);
  set_layout(&frame, 1920, 8194);
  check_frame("storage height above cap", &frame, -1);

  frame = valid_frame();
  set_layout(&frame, 2048, 1088);
  frame.crop_x = 1;
  check_frame("odd horizontal crop within storage bounds", &frame, -1);

  frame = valid_frame();
  frame.y_offset = SIZE_MAX;
  frame.uv_offset = SIZE_MAX;
  frame.allocation_bytes = SIZE_MAX;
  check_frame("huge offsets with huge allocation", &frame, -1);
}

int main(void)
{
  test_valid_frames();
  test_invalid_frames();

  if (failures != 0) {
    fprintf(stderr, "%u of %u frame checks failed\n", failures, checks);
    return EXIT_FAILURE;
  }
  printf("PASS: %u frame checks\n", checks);
  return EXIT_SUCCESS;
}
