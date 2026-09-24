/* Test actual presenter ordering with syscall stand-ins. This does not prove
 * that vendor timeline completion equals physical scanout completion. */
#define _POSIX_C_SOURCE 200809L
#include <poll.h>
int test_poll(struct pollfd *fds, nfds_t count, int timeout);
#define open test_open
#define close test_close
#define fcntl test_fcntl
#define ioctl test_ioctl
#define poll test_poll
#include "../../src/video/k2b/disp_presenter.c"
#undef open
#undef close
#undef fcntl
#undef ioctl
#undef poll
#include <stdarg.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL: %s\n", #x); exit(1); } } while (0)
static struct disp_layer_config2 observed;
static int acquired, waited_previous, early_blank, blanks, timeout_previous;
int test_open(const char *path, int flags, ...)
{ (void)path; (void)flags; return 3; }
int test_close(int fd) { (void)fd; return 0; }
int test_fcntl(int fd, int command, ...)
{ CHECK(command == F_DUPFD_CLOEXEC); return fd + 1000; }
int test_poll(struct pollfd *fds, nfds_t count, int timeout)
{
  (void)timeout; CHECK(count == 1);
  if (fds->fd == 1101) {
    if (timeout_previous) return 0;
    waited_previous = 1;
  }
  fds->revents = POLLIN;
  return 1;
}
int test_ioctl(int fd, unsigned long command, ...)
{
  (void)fd;
  va_list ap;
  va_start(ap, command);
  unsigned long *args = va_arg(ap, unsigned long *);
  va_end(ap);
  switch (command) {
  case DISP_DEVICE_GET_CONFIG: {
    struct disp_device_config *cfg = (void *)(uintptr_t)args[1];
    memset(cfg, 0, sizeof(*cfg));
    cfg->type = DISP_OUTPUT_TYPE_HDMI;
    cfg->mode = DISP_TV_MOD_1080P_60HZ;
    return 0;
  }
  case DISP_GET_OUTPUT_TYPE: return DISP_OUTPUT_TYPE_HDMI;
  case DISP_GET_SCN_WIDTH: return 1920;
  case DISP_GET_SCN_HEIGHT: return 1080;
  case DISP_LAYER_GET_CONFIG2:
    *(struct disp_layer_config2 *)(uintptr_t)args[1] = observed;
    return 0;
  case DISP_LAYER_SET_CONFIG2:
    observed = *(struct disp_layer_config2 *)(uintptr_t)args[1];
    if (!observed.enable) {
      blanks++;
      if (!waited_previous) early_blank++;
    }
    return 0;
  case DISP_HWC_COMMIT:
    if (args[1] == NEW_CLIENT) *(int *)(uintptr_t)args[2] = 432000000;
    if (args[1] == ACQUIRE_FENCE) {
      struct hwc_sync *s = (void *)(uintptr_t)args[2];
      s->count = ++acquired;
      s->fd = 100 + acquired;
    }
    return 0;
  default: CHECK(0); return -1;
  }
}
int k2b_disp_config_prepare(struct disp_layer_config2 *cfg,
                            const struct k2b_frame *frame, uint32_t id)
{
  (void)frame;
  memset(cfg, 0, sizeof(*cfg));
  cfg->enable = 1;
  cfg->info.id = id;
  cfg->info.fb.fd = 50 + id;
  cfg->info.fb.format = DISP_FORMAT_YUV420_SP_UVUV;
  return 0;
}
int main(int argc, char **argv)
{
  (void)argv;
  timeout_previous = argc > 1;
  struct k2b_disp *d = NULL;
  struct k2b_frame frame = {0};
  int fd = -1;
  CHECK(k2b_disp_open(&d) == 0);
  CHECK(k2b_disp_present(d, &frame, 1, &fd) == 0);
  CHECK(k2b_disp_present(d, &frame, 2, &fd) == 0);
  int result = k2b_disp_retire(d);
  if (timeout_previous) {
    CHECK(result == -1 && errno == ETIMEDOUT);
    CHECK(blanks == 0 && d->active);
    free(d); /* Test allocation only; no real device or descriptor exists. */
  } else {
    CHECK(result == 0);
    CHECK(waited_previous && early_blank == 0);
    CHECK(blanks == 3 && !d->active);
    k2b_disp_close(d);
  }
  puts("display retirement ordering: PASS");
  return 0;
}
