#include "cedar_picture.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned int checks, failures;
static char pixels[1920 * 1088 * 3 / 2 + 4096];

#define CHECK(name, condition) do { \
  checks++; \
  if (!(condition)) { \
    fprintf(stderr, "FAIL: %s (%s:%d)\n", name, __FILE__, __LINE__); \
    failures++; \
  } \
} while (0)

struct fixture {
  VideoPicture picture;
  struct k2b_cedar_buffer y, uv;
  enum k2b_matrix matrix;
  enum k2b_range range;
};

static struct fixture valid_fixture(void)
{
  struct fixture f = {0};
  f.picture.ePixelFormat = PIXEL_FORMAT_NV12;
  f.picture.nWidth = 1920;
  f.picture.nHeight = 1088;
  f.picture.nLineStride = 1920;
  f.picture.nRightOffset = 1920;
  f.picture.nBottomOffset = 1080;
  f.picture.bIsProgressive = 1;
  f.picture.pData0 = pixels;
  f.picture.pData1 = pixels + 1920 * 1088;
  f.picture.nBufSize = 1920 * 1088 * 3 / 2;
  f.y.base = pixels;
  f.y.bytes = sizeof(pixels);
  f.y.fd = 7;
  f.y.ve_address = 0x10000000;
  f.uv = f.y;
  f.uv.offset = 1920 * 1088;
  f.matrix = K2B_MATRIX_BT709;
  f.range = K2B_RANGE_LIMITED;
  return f;
}

static void rejected(const char *name, const VideoPicture *picture,
                     const struct k2b_cedar_buffer *y,
                     const struct k2b_cedar_buffer *uv,
                     enum k2b_matrix matrix, enum k2b_range range)
{
  struct k2b_frame out, before;
  memset(&out, 0xa5, sizeof(out));
  memcpy(&before, &out, sizeof(before));
  errno = 0;
  CHECK(name, k2b_cedar_picture_frame(picture, y, uv, matrix, range, &out) == -1);
  CHECK(name, errno == EINVAL);
  CHECK(name, memcmp(&out, &before, sizeof(out)) == 0);
}

static void invalid(const char *name, const struct fixture *f)
{
  rejected(name, &f->picture, &f->y, &f->uv, f->matrix, f->range);
}

static void accepted(const char *name, const struct fixture *f)
{
  struct k2b_frame out;
  memset(&out, 0xa5, sizeof(out));
  CHECK(name, k2b_cedar_picture_frame(&f->picture, &f->y, &f->uv,
                                    f->matrix, f->range, &out) == 0);
  CHECK(name, out.fd == f->y.fd && out.format == K2B_PIXEL_NV12);
  CHECK(name, out.allocation_bytes == f->y.bytes && out.y_offset == 0 &&
              out.uv_offset == f->uv.offset);
  CHECK(name, out.stride == (uint32_t)f->picture.nLineStride &&
              out.storage_height == (uint32_t)f->picture.nHeight);
  CHECK(name, out.crop_x == (uint32_t)f->picture.nLeftOffset &&
              out.crop_y == (uint32_t)f->picture.nTopOffset &&
              out.width == 1920 && out.height == 1080);
  CHECK(name, out.matrix == f->matrix && out.range == f->range);
  CHECK(name, k2b_frame_validate(&out) == 0);
}

#define INVALID(name, change) do { \
  struct fixture f = valid_fixture(); \
  change; \
  invalid(name, &f); \
} while (0)

static void test_valid(void)
{
  struct fixture f = valid_fixture();
  enum k2b_matrix matrices[] = {K2B_MATRIX_BT601, K2B_MATRIX_BT709};
  enum k2b_range ranges[] = {K2B_RANGE_LIMITED, K2B_RANGE_FULL};
  size_t i, j;
  accepted("baseline", &f);
  f.y.fd = f.uv.fd = 0;
  accepted("fd zero", &f);
  f.y.fd = f.uv.fd = INT_MAX;
  accepted("fd metadata only", &f);
  for (i = 0; i < sizeof(matrices) / sizeof(matrices[0]); i++) {
    for (j = 0; j < sizeof(ranges) / sizeof(ranges[0]); j++) {
      f.matrix = matrices[i];
      f.range = ranges[j];
      accepted("negotiated colors", &f);
    }
  }
  f = valid_fixture();
  f.picture.nBufFd = -1;
  f.picture.nColorPrimary = -1;
  f.picture.video_full_range_flag = (VIDEO_FULL_RANGE_FLAG)99;
  f.picture.matrix_coeffs = (VIDEO_MATRIX_COEFFS)99;
  f.picture.transfer_characteristics = (VIDEO_TRANSFER)99;
  f.picture.colour_primaries = 255;
  accepted("picture fd and colors are ignored", &f);
  f.picture.nBufFd = 123;
  accepted("different picture fd is ignored", &f);
  f = valid_fixture();
  f.picture.nBufSize = (int)f.y.bytes;
  accepted("buffer size includes padding", &f);
  f.picture.nBufSize = 1920 * 1088 * 3 / 2;
  f.y.bytes = f.uv.bytes = (size_t)f.picture.nBufSize;
  accepted("exact allocation size", &f);
  f = valid_fixture();
  f.picture.nLineStride = 1922;
  f.uv.offset = 1922 * 1088;
  f.picture.pData1 = pixels + f.uv.offset;
  f.picture.nBufSize = (int)(f.uv.offset * 3 / 2);
  accepted("width smaller than padded stride", &f);
  f.picture.nWidth = 1921;
  accepted("storage width need not equal the even stride", &f);
  f.picture.nWidth = 1922;
  f.picture.nLeftOffset = 2;
  f.picture.nRightOffset = 1922;
  f.picture.nTopOffset = 8;
  f.picture.nBottomOffset = 1088;
  accepted("larger even storage with crop at both bounds", &f);
}

static void test_invalid(void)
{
  struct fixture f = valid_fixture();
  rejected("null picture", NULL, &f.y, &f.uv, f.matrix, f.range);
  rejected("null Y view", &f.picture, NULL, &f.uv, f.matrix, f.range);
  rejected("null UV view", &f.picture, &f.y, NULL, f.matrix, f.range);
  errno = 0;
  CHECK("null output", k2b_cedar_picture_frame(&f.picture, &f.y, &f.uv,
                                               f.matrix, f.range, NULL) == -1);
  CHECK("null output errno", errno == EINVAL);
  INVALID("wrong format", f.picture.ePixelFormat = PIXEL_FORMAT_NV21);
  INVALID("nonzero stream", f.picture.nStreamIndex = 1);
  INVALID("negative stream", f.picture.nStreamIndex = -1);
  INVALID("interlaced", f.picture.bIsProgressive = 0);
  INVALID("invalid progressive flag", f.picture.bIsProgressive = 2);
  INVALID("negative progressive flag", f.picture.bIsProgressive = -1);
  INVALID("10 bit", f.picture.b10BitPicFlag = 1);
  INVALID("AFBC", f.picture.bEnableAfbcFlag = 1);
  INVALID("frame error", f.picture.bFrameErrorFlag = 1);
  INVALID("top field error", f.picture.bTopFieldError = 1);
  INVALID("bottom field error", f.picture.bBottomFieldError = 1);
  INVALID("extra plane 2", f.picture.pData2 = pixels);
  INVALID("extra plane 3", f.picture.pData3 = pixels);
  INVALID("zero width", f.picture.nWidth = 0);
  INVALID("negative width", f.picture.nWidth = -1);
  INVALID("width over cap", f.picture.nWidth = 8193);
  INVALID("zero height", f.picture.nHeight = 0);
  INVALID("negative height", f.picture.nHeight = -1);
  INVALID("height over cap", f.picture.nHeight = 8193);
  INVALID("zero stride", f.picture.nLineStride = 0);
  INVALID("negative stride", f.picture.nLineStride = -1);
  INVALID("stride over cap", f.picture.nLineStride = 8193);
  INVALID("width beyond stride", f.picture.nWidth = 1922);
  INVALID("negative left", f.picture.nLeftOffset = INT_MIN);
  INVALID("negative top", f.picture.nTopOffset = INT_MIN);
  INVALID("reversed horizontal crop", f.picture.nRightOffset = -1);
  INVALID("reversed vertical crop", f.picture.nBottomOffset = -1);
  INVALID("right beyond width", f.picture.nRightOffset = INT_MAX);
  INVALID("picture width below visible right", f.picture.nWidth = 1918);
  INVALID("bottom beyond height", f.picture.nBottomOffset = INT_MAX);
  INVALID("zero visible width", f.picture.nRightOffset = 0);
  INVALID("zero visible height", f.picture.nBottomOffset = 0);
  INVALID("wrong visible width", f.picture.nRightOffset = 1918);
  INVALID("wrong visible height", f.picture.nBottomOffset = 1078);
  INVALID("odd stride", f.picture.nLineStride = 1921;
          f.uv.offset = 1921 * 1088; f.picture.pData1 = pixels + f.uv.offset;
          f.picture.nBufSize = (int)(f.uv.offset * 3 / 2));
  INVALID("odd storage height", f.picture.nHeight = 1089;
          f.uv.offset = 1920 * 1089; f.picture.pData1 = pixels + f.uv.offset;
          f.picture.nBufSize = (int)(f.uv.offset * 3 / 2));
  INVALID("odd crop Y", f.picture.nTopOffset = 1; f.picture.nBottomOffset = 1081);
  INVALID("odd crop X", f.picture.nWidth = f.picture.nLineStride = 1922;
          f.picture.nLeftOffset = 1; f.picture.nRightOffset = 1921;
          f.uv.offset = 1922 * 1088; f.picture.pData1 = pixels + f.uv.offset;
          f.picture.nBufSize = (int)(f.uv.offset * 3 / 2));
  INVALID("zero matrix", f.matrix = 0);
  INVALID("unknown matrix", f.matrix = 3);
  INVALID("negative matrix", f.matrix = -1);
  INVALID("zero range", f.range = 0);
  INVALID("unknown range", f.range = 3);
  INVALID("negative range", f.range = -1);
  INVALID("null Y base", f.y.base = NULL);
  INVALID("null UV base", f.uv.base = NULL);
  INVALID("both bases null", f.y.base = f.uv.base = NULL;
          f.picture.pData0 = NULL; f.picture.pData1 = (char *)(uintptr_t)f.uv.offset);
  INVALID("different base", f.uv.base = pixels + 1);
  INVALID("negative Y fd", f.y.fd = -1);
  INVALID("negative UV fd", f.uv.fd = -1);
  INVALID("both fds negative", f.y.fd = f.uv.fd = -1);
  INVALID("different fd", f.uv.fd = 8);
  INVALID("different bytes", f.uv.bytes--);
  INVALID("different VE base", f.uv.ve_address++);
  INVALID("nonzero Y offset", f.y.offset = 2);
  INVALID("UV at allocation end", f.uv.offset = f.uv.bytes);
  INVALID("UV beyond allocation", f.uv.offset = SIZE_MAX);
  INVALID("UV before plane end", f.uv.offset -= 2;
          f.picture.pData1 = pixels + f.uv.offset);
  INVALID("UV after plane end", f.uv.offset += 2;
          f.picture.pData1 = pixels + f.uv.offset);
  INVALID("null Y pointer", f.picture.pData0 = NULL);
  INVALID("null UV pointer", f.picture.pData1 = NULL);
  INVALID("wrong Y pointer", f.picture.pData0 = pixels + 1);
  INVALID("wrong UV pointer", f.picture.pData1++);
  INVALID("zero allocation", f.y.bytes = f.uv.bytes = 0);
  INVALID("truncated allocation", f.y.bytes = f.uv.bytes = 1920 * 1088 * 3 / 2 - 1);
  INVALID("negative buffer size", f.picture.nBufSize = -1);
  INVALID("zero buffer size", f.picture.nBufSize = 0);
  INVALID("short buffer size", f.picture.nBufSize--);
  INVALID("buffer size beyond allocation", f.picture.nBufSize = (int)f.y.bytes + 1);
  INVALID("address range overflow", f.y.base = f.uv.base = (void *)(UINTPTR_MAX - 16);
          f.picture.pData0 = (char *)f.y.base;
          f.picture.pData1 = (char *)((uintptr_t)f.y.base + f.uv.offset));
}

int main(void)
{
  test_valid();
  test_invalid();
  if (failures != 0) {
    fprintf(stderr, "%u of %u picture checks failed\n", failures, checks);
    return EXIT_FAILURE;
  }
  printf("PASS: %u picture checks\n", checks);
  return EXIT_SUCCESS;
}
