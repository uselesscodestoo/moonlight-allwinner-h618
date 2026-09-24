#define _POSIX_C_SOURCE 200809L
#include "disp_presenter.h"
#include "disp_config.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

enum { SCREEN = 0, NEW_CLIENT = 1, DESTROY_CLIENT = 2,
       ACQUIRE_FENCE = 3, SUBMIT_FENCE = 4 };
struct hwc_sync { int fd; unsigned int count; };
struct k2b_disp {
  int fd, client, active;
  unsigned int sequence;
};

static int hwc(struct k2b_disp *d, unsigned int command, unsigned long value)
{
  unsigned long args[4] = {SCREEN, command, value, 0};
  return ioctl(d->fd, DISP_HWC_COMMIT, args);
}

static int layer(struct k2b_disp *d, unsigned long command,
                 struct disp_layer_config2 *config)
{
  unsigned long args[4] = {SCREEN, (unsigned long)(uintptr_t)config, 1, 0};
  return ioctl(d->fd, command, args);
}

static int query(struct k2b_disp *d, unsigned long command)
{
  unsigned long args[4] = {SCREEN, 0, 0, 0};
  return ioctl(d->fd, command, args);
}

static int wait_fence(int fd)
{
  struct pollfd p = {.fd = fd, .events = POLLIN};
  struct timespec start, now;
  int left = 1000;
  if (clock_gettime(CLOCK_MONOTONIC, &start) < 0) return -1;
  for (;;) {
    int rc = poll(&p, 1, left);
    if (rc > 0) {
      if ((p.revents & POLLIN) && !(p.revents & (POLLERR | POLLNVAL))) return 0;
      errno = EIO;
      return -1;
    }
    if (rc == 0) { errno = ETIMEDOUT; return -1; }
    if (errno != EINTR) return -1;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) return -1;
    left = 1000 - (int)((now.tv_sec - start.tv_sec) * 1000 +
                        (now.tv_nsec - start.tv_nsec) / 1000000);
    if (left <= 0) { errno = ETIMEDOUT; return -1; }
  }
}

/* SET_CONFIG2 and HWC_SUBMIT are separate vendor operations, not an atomic KMS
 * commit. Keep one submitter and test this vendor path on the real board. */
static int commit(struct k2b_disp *d, struct disp_layer_config2 *config, int *fd)
{
  struct hwc_sync sync = {.fd = -1, .count = 0};
  struct disp_layer_config2 observed = {.channel = 0, .layer_id = 0};
  if (hwc(d, ACQUIRE_FENCE, (unsigned long)(uintptr_t)&sync) < 0) return -1;
  if (sync.fd < 0 || sync.count == 0 || sync.count <= d->sequence) {
    if (sync.fd >= 0) close(sync.fd);
    errno = EPROTO;
    return -1;
  }
  d->sequence = sync.count;
  d->active = 1; /* A failed SET may still have changed the hardware. */
  if (layer(d, DISP_LAYER_SET_CONFIG2, config) < 0 ||
      layer(d, DISP_LAYER_GET_CONFIG2, &observed) < 0) goto failed;
  if (observed.enable != config->enable ||
      (config->enable && (observed.info.fb.fd != config->info.fb.fd ||
       observed.info.fb.format != config->info.fb.format ||
       observed.info.id != config->info.id))) {
    errno = EIO;
    goto failed;
  }
  if (hwc(d, SUBMIT_FENCE, sync.count) < 0) goto failed;
  *fd = sync.fd;
  return 0;
failed:
  {
    int error = errno;
    close(sync.fd);
    errno = error;
    return -1;
  }
}

int k2b_disp_open(struct k2b_disp **out)
{
  struct k2b_disp *d;
  struct disp_device_config output = {0};
  struct disp_layer_config2 previous = {.channel = 0, .layer_id = 0};
  int clock_rate = -1, type;
  unsigned long args[4];
  if (!out || *out) { errno = EINVAL; return -1; }
  d = calloc(1, sizeof(*d));
  if (!d) return -1;
  d->fd = open("/dev/disp", O_RDWR | O_CLOEXEC);
  if (d->fd < 0) goto failed;
  args[0] = SCREEN; args[1] = (unsigned long)(uintptr_t)&output;
  args[2] = 0; args[3] = 0;
  if (ioctl(d->fd, DISP_DEVICE_GET_CONFIG, args) < 0) goto failed;
  if (output.type != DISP_OUTPUT_TYPE_HDMI || output.mode != DISP_TV_MOD_1080P_60HZ) {
    errno = ENOTSUP;
    goto failed;
  }
  type = query(d, DISP_GET_OUTPUT_TYPE);
  if (type == DISP_OUTPUT_TYPE_NONE) {
    if (ioctl(d->fd, DISP_DEVICE_SET_CONFIG, args) < 0) goto failed;
    type = query(d, DISP_GET_OUTPUT_TYPE);
  }
  if (type != DISP_OUTPUT_TYPE_HDMI || query(d, DISP_GET_SCN_WIDTH) != 1920 ||
      query(d, DISP_GET_SCN_HEIGHT) != 1080) { errno = ENOTSUP; goto failed; }
  if (layer(d, DISP_LAYER_GET_CONFIG2, &previous) < 0) goto failed;
  if (previous.enable) { errno = EBUSY; goto failed; }
  if (hwc(d, NEW_CLIENT, (unsigned long)(uintptr_t)&clock_rate) < 0) goto failed;
  /* Existing global client returns success without writing clock_rate. */
  if (clock_rate <= 0) { errno = EBUSY; goto failed; }
  d->client = 1;
  fprintf(stderr, "K2B: HDMI 1920x1080@60, composer clock=%d\n", clock_rate);
  *out = d;
  return 0;
failed:
  {
    int error = errno;
    if (d->client) hwc(d, DESTROY_CLIENT, 0);
    if (d->fd >= 0) close(d->fd);
    free(d);
    errno = error;
    return -1;
  }
}

int k2b_disp_present(struct k2b_disp *d, const struct k2b_frame *frame,
                     uint32_t frame_id, int *release_fd)
{
  struct disp_layer_config2 config;
  if (!d || !release_fd || k2b_disp_config_prepare(&config, frame, frame_id) < 0) {
    errno = EINVAL;
    return -1;
  }
  return commit(d, &config, release_fd);
}

int k2b_disp_retire(struct k2b_disp *d)
{
  struct disp_layer_config2 blank = {.channel = 0, .layer_id = 0};
  int previous = -1, next = -1;
  if (!d) { errno = EINVAL; return -1; }
  /* A vendor fence signals only after a later sequence. Three blank commits
   * provide two observed transitions and a later SET to reap old imports.
   * No sleeps stand in for fence readiness. This is a practical vendor-path
   * drain, not a proof of all RCQ corner cases. */
  for (int i = 0; i < 3; ++i) {
    if (commit(d, &blank, &next) < 0) goto failed;
    if (previous >= 0) {
      if (wait_fence(previous) < 0) goto failed;
      close(previous);
    }
    previous = next;
    next = -1;
  }
  close(previous); /* Last blank fence has no frame associated with it. */
  d->active = 0;
  return 0;
failed:
  {
    int error = errno;
    if (previous >= 0) close(previous);
    if (next >= 0) close(next);
    errno = error;
    return -1;
  }
}

void k2b_disp_close(struct k2b_disp *d)
{
  if (!d) return;
  if (d->active) {
    fprintf(stderr, "K2B: refusing display close before successful layer drain\n");
    return;
  }
  if (d->client) hwc(d, DESTROY_CLIENT, 0);
  close(d->fd);
  free(d);
}
