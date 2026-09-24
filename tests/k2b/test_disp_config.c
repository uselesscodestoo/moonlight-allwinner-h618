#include "disp_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned int checks;
static unsigned int failures;

#define CHECK(condition) do { \
  checks++; \
  if (!(condition)) { \
    fprintf(stderr, "FAIL: line %d: %s\n", __LINE__, #condition); \
    failures++; \
  } \
} while (0)

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

static void set_layout(struct k2b_frame *frame, uint32_t stride,
                       uint32_t storage_height)
{
  frame->stride = stride;
  frame->storage_height = storage_height;
  frame->uv_offset = (size_t)stride * storage_height;
  frame->allocation_bytes = frame->uv_offset + frame->uv_offset / 2;
}

static void check_disabled_features(const struct disp_layer_config2 *config)
{
  const struct disp_fb_info2 *fb = &config->info.fb;
  const struct disp_snr_info *snr = &config->info.snr;

  CHECK(fb->size[2].width == 0 && fb->size[2].height == 0);
  CHECK(fb->align[0] == 0 && fb->align[1] == 0 && fb->align[2] == 0);
  CHECK(fb->trd_right_fd == -1);
  CHECK(fb->pre_multiply == false);
  CHECK(fb->depth == 0);
  CHECK(fb->fbd_en == 0 && fb->lbc_en == 0);
  CHECK(fb->lbc_info.is_lossy == 0 && fb->lbc_info.rc_en == 0);
  CHECK(fb->lbc_info.pitch == 0 && fb->lbc_info.seg_bit == 0);
  CHECK(fb->metadata_fd == -1);
  CHECK(fb->metadata_size == 0 && fb->metadata_flag == 0);
  CHECK(config->info.b_trd_out == false && config->info.out_trd_mode == 0);
  CHECK(config->info.atw.used == false && config->info.atw.mode == 0);
  CHECK(config->info.atw.b_row == 0 && config->info.atw.b_col == 0);
  CHECK(config->info.atw.cof_fd == -1);
  CHECK(config->info.transform == 0);
  CHECK(snr->en == 0 && snr->demo_en == 0);
  CHECK(snr->demo_win.x == 0 && snr->demo_win.y == 0);
  CHECK(snr->demo_win.width == 0 && snr->demo_win.height == 0);
  CHECK(snr->y_strength == 0 && snr->u_strength == 0 && snr->v_strength == 0);
  CHECK(snr->th_ver_line == 0 && snr->th_hor_line == 0);
}

static void check_valid(const struct k2b_frame *frame, uint32_t frame_id,
                        enum disp_color_space expected_color)
{
  struct disp_layer_config2 config;
  struct k2b_frame original;
  int result;

  memcpy(&original, frame, sizeof(original));
  memset(&config, 0xa5, sizeof(config));
  result = k2b_disp_config_prepare(&config, frame, frame_id);
  CHECK(result == 0);
  CHECK(memcmp(frame, &original, sizeof(original)) == 0);
  if (result != 0)
    return;

  CHECK(config.enable == true);
  CHECK(config.channel == 0 && config.layer_id == 0);
  CHECK(config.info.mode == LAYER_MODE_BUFFER);
  CHECK(config.info.zorder == 31);
  CHECK(config.info.alpha_mode == 1 && config.info.alpha_value == 255);
  CHECK(config.info.screen_win.x == 0 && config.info.screen_win.y == 0);
  CHECK(config.info.screen_win.width == 1920);
  CHECK(config.info.screen_win.height == 1080);
  CHECK(config.info.id == frame_id);
  CHECK(config.info.fb.fd == frame->fd);
  CHECK(config.info.fb.size[0].width == frame->stride);
  CHECK(config.info.fb.size[0].height == frame->storage_height);
  CHECK(config.info.fb.size[1].width == frame->stride / 2);
  CHECK(config.info.fb.size[1].height == frame->storage_height / 2);
  CHECK(config.info.fb.format == DISP_FORMAT_YUV420_SP_UVUV);
  CHECK(config.info.fb.color_space == expected_color);
  CHECK(config.info.fb.eotf == DISP_EOTF_GAMMA22);
  CHECK(config.info.fb.flags == DISP_BF_NORMAL);
  CHECK(config.info.fb.scan == DISP_SCAN_PROGRESSIVE);
  CHECK(config.info.fb.crop.x == (int64_t)frame->crop_x * INT64_C(4294967296));
  CHECK(config.info.fb.crop.y == (int64_t)frame->crop_y * INT64_C(4294967296));
  CHECK(config.info.fb.crop.width == INT64_C(8246337208320));
  CHECK(config.info.fb.crop.height == INT64_C(4638564679680));
  check_disabled_features(&config);
}

static void test_valid_frames(void)
{
  struct k2b_frame frame = valid_frame();

  /* 1920x1088 Y and 960x544 UV storage, but only 1920x1080 is visible. */
  check_valid(&frame, 0, DISP_BT709);
  check_valid(&frame, UINT32_MAX, DISP_BT709);

  set_layout(&frame, 1920, 1080);
  check_valid(&frame, 42, DISP_BT709);
  set_layout(&frame, 2048, 1088);
  frame.fd = 17;
  check_valid(&frame, 43, DISP_BT709);
  frame.crop_x = 128;
  frame.crop_y = 8;
  check_valid(&frame, 44, DISP_BT709);

  frame = valid_frame();
  frame.matrix = K2B_MATRIX_BT601;
  check_valid(&frame, 1, DISP_BT601);
  frame.range = K2B_RANGE_FULL;
  check_valid(&frame, 2, DISP_BT601_F);
  frame.matrix = K2B_MATRIX_BT709;
  check_valid(&frame, 3, DISP_BT709_F);
}

static void check_invalid(const struct k2b_frame *frame)
{
  struct disp_layer_config2 config;
  unsigned char original[sizeof(config)];

  memset(&config, 0xa5, sizeof(config));
  memcpy(original, &config, sizeof(config));
  CHECK(k2b_disp_config_prepare(&config, frame, 77) == -1);
  CHECK(memcmp(original, &config, sizeof(config)) == 0);
}

#define CHECK_INVALID(field, value) do { \
  struct k2b_frame frame = valid_frame(); \
  frame.field = (value); \
  check_invalid(&frame); \
} while (0)

static void test_invalid_frames(void)
{
  struct k2b_frame frame = valid_frame();

  CHECK(k2b_disp_config_prepare(NULL, &frame, 0) == -1);
  CHECK(k2b_disp_config_prepare(NULL, NULL, 0) == -1);
  check_invalid(NULL);
  CHECK_INVALID(fd, -1);
  CHECK_INVALID(format, 0);
  CHECK_INVALID(y_offset, 2);
  CHECK_INVALID(uv_offset, (size_t)1920 * 1088 - 1);
  CHECK_INVALID(uv_offset, (size_t)1920 * 1088 + 1);
  CHECK_INVALID(allocation_bytes, (size_t)1920 * 1088 * 3 / 2 - 1);
  CHECK_INVALID(width, 1918);
  CHECK_INVALID(height, 1078);
  CHECK_INVALID(matrix, 0);
  CHECK_INVALID(matrix, 3);
  CHECK_INVALID(range, 0);
  CHECK_INVALID(range, 3);
  CHECK_INVALID(stride, 1921);
  CHECK_INVALID(storage_height, 1089);
  CHECK_INVALID(crop_x, 2);
  CHECK_INVALID(crop_y, 1);
}

int main(void)
{
  test_valid_frames();
  test_invalid_frames();
  if (failures != 0) {
    fprintf(stderr, "%u of %u disp config checks failed\n", failures, checks);
    return EXIT_FAILURE;
  }
  printf("PASS: %u disp config checks\n", checks);
  return EXIT_SUCCESS;
}
