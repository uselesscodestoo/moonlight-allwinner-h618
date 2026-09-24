#define _POSIX_C_SOURCE 200809L

#include "cedar_runtime.h"
#include "cedar_picture.h"
#include "disp_presenter.h"
#include "input_queue.h"
#include "../video.h"
#include "../../connection.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define K2B_HELD_MAX 5
#define K2B_SBM_FRAMES 8

struct held_picture {
  VideoPicture *picture;
  struct k2b_cedar_pin pin;
  int pinned;
  int release_fd;
  uint64_t submitted_at;
};

struct k2b_video {
  pthread_t thread;
  pthread_mutex_t mutex;
  pthread_cond_t ready_cond;
  struct k2b_input_queue *queue;
  int ready;
  int initialized;
  int hevc;
  int memory_started;
  int stop;
  int failed;
  enum k2b_range range;
  const struct k2b_cedar_api *api;
  VideoDecoder *decoder;
  struct k2b_disp *disp;
  struct held_picture held[K2B_HELD_MAX];
  unsigned held_count;
  VideoPicture *pending_picture; /* Decoded, never imported by DE yet. */
  uint64_t received, submitted, decoded, displayed, released;
  uint64_t decoded_at_reset;
  uint64_t started_ms;
  uint64_t last_input_ms, last_idr_request_ms;
  double decode_ms;
  enum k2b_matrix matrix;
  int diagnostic_static;
  int recovering, waiting_idr, diagnostic_drop;
  unsigned recoveries;
};

static struct k2b_video *active;
static enum k2b_range configured_range = K2B_RANGE_LIMITED;

void video_k2b_configure(int color_range)
{
  configured_range = color_range == COLOR_RANGE_FULL ? K2B_RANGE_FULL : K2B_RANGE_LIMITED;
}

static uint64_t monotonic_ms(void)
{
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
}

static void idle_tick(void)
{
  const struct timespec delay = { .tv_nsec = 1000000 };
  nanosleep(&delay, NULL);
}

static int memory_healthy(struct k2b_video *video, const char *operation)
{
  struct k2b_cedar_memory_status status;
  if (video->api->memory_status(&status) == 0 && !status.error)
    return 1;
  fprintf(stderr, "K2B: Cedar memory error after %s\n", operation);
  return 0;
}

static int requested_stop(struct k2b_video *video)
{
  int stop;
  pthread_mutex_lock(&video->mutex);
  stop = video->stop;
  pthread_mutex_unlock(&video->mutex);
  return stop;
}

static void fail_worker(struct k2b_video *video, const char *operation)
{
  fprintf(stderr, "K2B: %s failed (%s); stopping video and requiring a new connection\n",
          operation, strerror(errno));
  pthread_mutex_lock(&video->mutex);
  video->failed = 1;
  video->stop = 1;
  pthread_mutex_unlock(&video->mutex);
  k2b_input_queue_stop(video->queue);
}

/* Count complete decode units, not incoming UDP fragments. A stream which
 * worked once must not remain frozen forever while incomplete packets arrive. */
static int check_input_timeout(struct k2b_video *video)
{
  uint64_t now = monotonic_ms();
  pthread_mutex_lock(&video->mutex);
  uint64_t last = video->last_input_ms;
  int stopping = video->stop;
  pthread_mutex_unlock(&video->mutex);
  if (stopping) return 0;
  if (!last) last = video->started_ms;
  uint64_t silence = now - last;
  if (silence >= 10000) {
    errno = ETIMEDOUT;
    fail_worker(video, "no complete video frame for 10 seconds");
    /* Wake the existing signalfd main loop; never call LiStopConnection from
     * this worker, because cleanup must join us. Preserve normal DMA retirement. */
    if (main_thread_id != 0) pthread_kill(main_thread_id, SIGTERM);
    return -1;
  }
  if (silence >= 2000 && now - video->last_idr_request_ms >= 2000) {
    video->last_idr_request_ms = now;
    LiRequestIdrFrame();
    fprintf(stderr, "K2B: no complete video frame for %llu ms; requesting IDR\n",
            (unsigned long long)silence);
  }
  return 0;
}

static int release_old_picture(struct k2b_video *video, unsigned index)
{
  struct held_picture *held = &video->held[index];
  if (held->picture) {
    if (video->api->return_picture(video->decoder, held->picture) < 0)
      return -1;
    held->picture = NULL;
    if (!memory_healthy(video, "ReturnPicture"))
      return -1;
  }
  if (held->pinned && video->api->memory_unpin(&held->pin) < 0)
    return -1;
  if (held->release_fd >= 0)
    close(held->release_fd);
  memmove(held, held + 1, (video->held_count - index - 1) * sizeof(*held));
  video->held_count--;
  video->released++;
  return 0;
}

static int release_signaled(struct k2b_video *video)
{
  unsigned index = 0;
  while (index < video->held_count) {
    struct held_picture *held = &video->held[index];
    /* A later submitted frame must have reached the vendor timeline. */
    if (held->release_fd < 0 || video->displayed - held->submitted_at < 1) {
      index++;
      continue;
    }
    struct pollfd pfd = { .fd = held->release_fd, .events = POLLIN };
    int polled = poll(&pfd, 1, 0);
    if (polled < 0 && errno == EINTR) { index++; continue; }
    if (polled < 0 || (polled > 0 && (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))))
      return -1;
    if (polled == 0 || !(pfd.revents & POLLIN)) {
      index++;
      continue;
    }
    if (release_old_picture(video, index) < 0)
      return -1;
  }
  return 0;
}

static int submit_input(struct k2b_video *video, const struct k2b_input_view *view)
{
  int frames = video->api->stream_frames(video->decoder, 0);
  if (frames < 0 || !memory_healthy(video, "VideoStreamFrameNum"))
    return -1;
  /* Cedar's parsed-stream count is not our submitted-AU count: the paced
   * sample reports 8 after one AU and still needs the next AU to decode.
   * Bypass the latency watermark until the first picture after init/reset;
   * RequestVideoStreamBuffer still enforces actual capacity. Once decoding
   * starts, preserve the previously tested steady-state throttling. */
  if (video->decoded > video->decoded_at_reset && frames >= K2B_SBM_FRAMES)
    return 0;

  enum k2b_matrix matrix = view->unit.colorspace == COLORSPACE_REC_601 ?
      K2B_MATRIX_BT601 : K2B_MATRIX_BT709;
  if (video->matrix && video->matrix != matrix) {
    errno = EINVAL;
    fprintf(stderr, "K2B: midstream colorspace change requires a new connection\n");
    return -1;
  }
  video->matrix = matrix;

  char *first = NULL, *ring = NULL;
  int first_bytes = 0, ring_bytes = 0;
  int bytes = (int)view->unit.bytes;
  int requested = video->api->request_stream(video->decoder, bytes,
      &first, &first_bytes, &ring, &ring_bytes, 0);
  if (!memory_healthy(video, "RequestVideoStreamBuffer"))
    return -1;
  if (requested < 0)
    return 0; /* Healthy negative result is SBM backpressure. */
  if (!first || first_bytes < 0 || ring_bytes < 0 ||
      (size_t)first_bytes + (size_t)ring_bytes < view->unit.bytes ||
      (first_bytes < bytes && !ring)) {
    errno = EIO;
    return -1;
  }
  int first_copy = first_bytes < bytes ? first_bytes : bytes;
  memcpy(first, view->data, (size_t)first_copy);
  if (first_copy < bytes)
    memcpy(ring, view->data + first_copy, (size_t)(bytes - first_copy));
  VideoStreamDataInfo stream = { .pData = first, .nLength = bytes,
      .nPts = view->unit.pts_us, .nPcr = -1,
      .bIsFirstPart = 1, .bIsLastPart = 1, .bValid = 1 };
  if (video->api->submit_stream(video->decoder, &stream, 0) < 0 ||
      !memory_healthy(video, "SubmitVideoStreamData"))
    return -1;
  video->submitted++;
  return 1;
}

static int present_picture(struct k2b_video *video, VideoPicture *picture)
{
  /* Diagnostic only: hold the first decoded picture to isolate buffer flips. */
  if (video->diagnostic_static && video->displayed)
    return video->api->return_picture(video->decoder, picture);
  if (video->held_count >= K2B_HELD_MAX) {
    errno = ENOBUFS;
    return -1;
  }
  struct held_picture *held = &video->held[video->held_count++];
  memset(held, 0, sizeof(*held));
  held->picture = picture;
  held->release_fd = -1;

  struct k2b_cedar_buffer y, uv;
  struct k2b_frame frame;
  if (video->api->memory_describe(picture->pData0, &y) < 0 ||
      video->api->memory_describe(picture->pData1, &uv) < 0 ||
      !memory_healthy(video, "describe picture") ||
      video->api->memory_pin(picture->pData0, &held->pin) < 0)
    return -1;
  held->pinned = 1;
  if (k2b_cedar_picture_frame(picture, &y, &uv, video->matrix, video->range, &frame) < 0)
    return -1;
  if (!video->displayed)
    fprintf(stderr, "K2B: first NV12 picture %ux%u stride=%u storage_height=%u fd=%d bytes=%zu\n",
            frame.width, frame.height, frame.stride, frame.storage_height,
            frame.fd, y.bytes);
  if (k2b_disp_present(video->disp, &frame, (uint32_t)video->decoded,
                       &held->release_fd) < 0)
    return -1;
  video->displayed++;
  held->submitted_at = video->displayed;
  return 0;
}

static int present_pending_picture(struct k2b_video *video)
{
  if (!video->pending_picture || video->held_count >= 2)
    return 0;
  VideoPicture *picture = video->pending_picture;
  video->pending_picture = NULL;
  /* present_picture takes ownership even if import/commit subsequently fails. */
  return present_picture(video, picture);
}

static int return_pending_picture(struct k2b_video *video)
{
  if (!video->pending_picture)
    return 0;
  if (video->api->return_picture(video->decoder, video->pending_picture) < 0)
    return -1;
  video->pending_picture = NULL;
  return memory_healthy(video, "ReturnPicture pending") ? 0 : -1;
}

static int initialize_worker(struct k2b_video *video)
{
  video->diagnostic_static = getenv("K2B_DIAGNOSTIC_STATIC") != NULL;
  video->diagnostic_drop = getenv("K2B_DIAGNOSTIC_DROP_FRAME") != NULL;
  const char *directory = getenv("K2B_CEDAR_RUNTIME_DIR");
  if (!directory || !*directory ||
      k2b_cedar_runtime_load(directory, &video->api) < 0)
    return -1;
  if (video->api->memory_begin() < 0)
    return -1;
  video->memory_started = 1;
  video->decoder = video->api->create();
  if (!video->decoder || !memory_healthy(video, "CreateVideoDecoder"))
    return -1;
  VideoStreamInfo stream = {
      .eCodecFormat = video->hevc ? VIDEO_CODEC_FORMAT_H265 : VIDEO_CODEC_FORMAT_H264,
      .nWidth = 1920, .nHeight = 1080, .nFrameRate = 60000,
      .nFrameDuration = 16667, .bIsFramePackage = 1 };
  VConfig config = { .eOutputPixelFormat = PIXEL_FORMAT_NV12,
      .bDisable3D = 1, .nVbvBufferSize = 8 * 1024 * 1024,
      .nAlignStride = 16, .eCtlAfbcMode = DISABLE_AFBC_ALL_SIZE,
      .nDisplayHoldingFrameBufferNum = 4, .nDecodeSmoothFrameBufferNum = 1,
      .memops = video->api->mem_ops() };
  if (!config.memops ||
      video->api->initialize(video->decoder, &stream, &config) < 0 ||
      !memory_healthy(video, "InitializeVideoDecoder") ||
      k2b_disp_open(&video->disp) < 0)
    return -1;
  return 0;
}

static void finish_worker(struct k2b_video *video)
{
  /* Retirement is mandatory even after a failed present: its input picture
   * may have reached scanout without a usable release fence. */
  if (video->disp && k2b_disp_retire(video->disp) < 0) {
    fprintf(stderr, "K2B: display retirement failed; retaining decoder and buffers. Board recovery required.\n");
    return;
  }
  if (return_pending_picture(video) < 0) {
    fprintf(stderr, "K2B: pending picture release failed; board recovery required.\n");
    return;
  }
  while (video->held_count)
    if (release_old_picture(video, 0) < 0) {
      fprintf(stderr, "K2B: picture release failed; retaining decoder. Board recovery required.\n");
      return;
    }
  if (video->decoder) {
    video->api->destroy(video->decoder);
    video->decoder = NULL;
    if (!memory_healthy(video, "DestroyVideoDecoder"))
      return;
  }
  if (video->api && video->memory_started) {
    struct k2b_cedar_memory_status status;
    if (video->api->memory_status(&status) < 0 || status.error ||
        status.allocations || status.pinned || status.references ||
        video->api->memory_end() < 0) {
      fprintf(stderr, "K2B: Cedar memory did not drain; board recovery required.\n");
      return;
    }
  }
  if (video->disp)
    k2b_disp_close(video->disp);
}

static void print_stats(struct k2b_video *video)
{
  struct k2b_input_stats queue;
  pthread_mutex_lock(&video->mutex);
  uint64_t received = video->received;
  pthread_mutex_unlock(&video->mutex);
  if (k2b_input_queue_stats(video->queue, &queue) == K2B_QUEUE_OK)
    fprintf(stderr, "K2B: received=%llu enqueued=%llu submitted=%llu decoded=%llu display_submitted=%llu released=%llu held=%u queued=%zu decode_ms=%.1f elapsed_ms=%llu recoveries=%u discarded=%llu queue_peak=%zu pending=%u\n",
        (unsigned long long)received, (unsigned long long)queue.accepted,
        (unsigned long long)video->submitted, (unsigned long long)video->decoded,
        (unsigned long long)video->displayed, (unsigned long long)video->released,
        video->held_count, queue.queued, video->decode_ms,
        (unsigned long long)(monotonic_ms() - video->started_ms),
        video->recoveries, (unsigned long long)queue.discarded, queue.high_watermark,
        video->pending_picture != NULL);
}

static void *video_worker(void *opaque)
{
  struct k2b_video *video = opaque;
  int ok = initialize_worker(video) == 0;
  pthread_mutex_lock(&video->mutex);
  video->initialized = ok;
  video->ready = 1;
  pthread_cond_signal(&video->ready_cond);
  pthread_mutex_unlock(&video->mutex);
  if (!ok) {
    fail_worker(video, "decoder/display initialization");
    finish_worker(video);
    return NULL;
  }

  struct k2b_input_view input = { 0 };
  int leased = 0;
  video->started_ms = monotonic_ms();
  uint64_t next_stats = monotonic_ms() + 5000;
  while (!requested_stop(video)) {
    if (check_input_timeout(video) < 0) break;
    int progress = 0;
    pthread_mutex_lock(&video->mutex);
    int recovering = video->recovering;
    pthread_mutex_unlock(&video->mutex);
    if (recovering) {
      if (leased) {
        k2b_input_queue_release(video->queue, &input);
        leased = 0;
      }
      k2b_input_queue_discard_pending(video->queue);
      if (k2b_disp_retire(video->disp) < 0) {
        fail_worker(video, "recovery display drain"); break;
      }
      if (return_pending_picture(video) < 0) {
        fail_worker(video, "recovery pending picture release"); break;
      }
      while (video->held_count)
        if (release_old_picture(video, 0) < 0) {
          fail_worker(video, "recovery picture release"); break;
        }
      if (requested_stop(video)) break;
      video->api->reset(video->decoder);
      if (!memory_healthy(video, "ResetVideoDecoder")) {
        fail_worker(video, "decoder reset"); break;
      }
      video->matrix = 0;
      video->decoded_at_reset = video->decoded;
      pthread_mutex_lock(&video->mutex);
      video->recovering = 0;
      video->waiting_idr = 1;
      pthread_mutex_unlock(&video->mutex);
      fprintf(stderr, "K2B: recovery %u reset complete; waiting for IDR\n", ++video->recoveries);
      continue;
    }
    if (release_signaled(video) < 0) { fail_worker(video, "release fence/picture"); break; }
    /* Keep the two-import display limit, but prepare one next picture while
     * waiting for retirement. Decode must not start only after the flip. */
    if (present_pending_picture(video) < 0) {
      fail_worker(video, "pending picture presentation"); break;
    }
    if (video->pending_picture) goto wait_or_report;
    if (!leased) {
      int result = k2b_input_queue_take(video->queue, &input, 0);
      if (result == K2B_QUEUE_OK) leased = 1;
      else if (result != K2B_QUEUE_EMPTY && result != K2B_QUEUE_STOPPED) {
        fail_worker(video, "input dequeue"); break;
      }
    }
    if (leased) {
      int result = submit_input(video, &input);
      if (result < 0) { fail_worker(video, "compressed AU submission"); break; }
      if (result > 0) {
        if (k2b_input_queue_release(video->queue, &input) != K2B_QUEUE_OK) {
          fail_worker(video, "input release"); break;
        }
        leased = 0;
        progress = 1;
      }
    }
    uint64_t start = monotonic_ms();
    int result = video->api->decode(video->decoder, 0, 0, 0, 0);
    video->decode_ms += (double)(monotonic_ms() - start);
    if (result < 0 || result == VDECODE_RESULT_RESOLUTION_CHANGE ||
        result > VDECODE_RESULT_RESOLUTION_CHANGE ||
        !memory_healthy(video, "DecodeVideoStream")) {
      fail_worker(video, "DecodeVideoStream"); break;
    }
    if (video->submitted) {
      VideoPicture *picture = video->api->request_picture(video->decoder, 0);
      if (!memory_healthy(video, "RequestPicture")) {
        fail_worker(video, "RequestPicture"); break;
      }
      if (picture) {
        video->decoded++;
        video->pending_picture = picture;
        progress = 1;
        if (present_pending_picture(video) < 0) {
          fail_worker(video, "picture presentation"); break;
        }
      }
    }
wait_or_report:
    if (requested_stop(video)) break;
    if (monotonic_ms() >= next_stats) {
      print_stats(video);
      next_stats = monotonic_ms() + 5000;
    }
    if (!progress) idle_tick();
  }
  if (leased)
    k2b_input_queue_release(video->queue, &input);
  print_stats(video);
  finish_worker(video);
  return NULL;
}

static int k2b_setup(int format, int width, int height, int refresh,
                     void *context, int flags)
{
  (void)context;
  if (active || (format != VIDEO_FORMAT_H264 && format != VIDEO_FORMAT_H265) || width != 1920 ||
      height != 1080 || refresh != 60 || (flags & DISPLAY_ROTATE_MASK)) {
    fprintf(stderr, "K2B: supports only H.264/H.265 8-bit SDR 1920x1080 at 60 Hz without rotation\n");
    return -1;
  }
  struct k2b_video *video = calloc(1, sizeof(*video));
  if (!video) return -1;
  video->hevc = format == VIDEO_FORMAT_H265;
  fprintf(stderr, "K2B: selected %s hardware decoder, NV12 1920x1080 at 60 Hz\n",
          video->hevc ? "H.265" : "H.264");
  video->range = configured_range;
  if (pthread_mutex_init(&video->mutex, NULL) != 0) { free(video); return -1; }
  if (pthread_cond_init(&video->ready_cond, NULL) != 0) {
    pthread_mutex_destroy(&video->mutex); free(video); return -1;
  }
  if (k2b_input_queue_create(&video->queue, 8, K2B_AU_MAX_BYTES) != K2B_QUEUE_OK) {
    pthread_cond_destroy(&video->ready_cond);
    pthread_mutex_destroy(&video->mutex); free(video); return -1;
  }
  if (pthread_create(&video->thread, NULL, video_worker, video) != 0) {
    k2b_input_queue_stop(video->queue);
    k2b_input_queue_destroy(&video->queue);
    pthread_cond_destroy(&video->ready_cond);
    pthread_mutex_destroy(&video->mutex); free(video); return -1;
  }
  pthread_mutex_lock(&video->mutex);
  while (!video->ready)
    pthread_cond_wait(&video->ready_cond, &video->mutex);
  int initialized = video->initialized;
  pthread_mutex_unlock(&video->mutex);
  if (!initialized) {
    pthread_join(video->thread, NULL);
    k2b_input_queue_destroy(&video->queue);
    pthread_cond_destroy(&video->ready_cond);
    pthread_mutex_destroy(&video->mutex); free(video); return -1;
  }
  active = video;
  return 0;
}

static void k2b_stop(void)
{
  struct k2b_video *video = active;
  if (!video) return;
  pthread_mutex_lock(&video->mutex);
  video->stop = 1;
  pthread_mutex_unlock(&video->mutex);
  k2b_input_queue_stop(video->queue);
}

static void k2b_cleanup(void)
{
  struct k2b_video *video = active;
  if (!video) return;
  k2b_stop();
  pthread_join(video->thread, NULL);
  k2b_input_queue_destroy(&video->queue);
  pthread_cond_destroy(&video->ready_cond);
  pthread_mutex_destroy(&video->mutex);
  active = NULL;
  free(video);
}

static int k2b_submit(PDECODE_UNIT unit)
{
  struct k2b_video *video = active;
  if (!video || !unit) return DR_NEED_IDR;
  pthread_mutex_lock(&video->mutex);
  int stopped = video->stop || video->failed;
  if (!stopped) {
    video->received++;
    video->last_input_ms = monotonic_ms();
  }
  if (stopped || video->recovering ||
      (video->waiting_idr && unit->frameType != FRAME_TYPE_IDR)) {
    pthread_mutex_unlock(&video->mutex);
    return DR_NEED_IDR;
  }
  int injected = video->diagnostic_drop && video->received == 120;
  int result = injected ? K2B_QUEUE_FULL : k2b_input_queue_push(video->queue, unit);
  if (result == K2B_QUEUE_OK) {
    if (video->waiting_idr)
      fprintf(stderr, "K2B: IDR accepted; resuming video\n");
    video->waiting_idr = 0;
    pthread_mutex_unlock(&video->mutex);
    return DR_OK;
  }
  video->recovering = 1;
  video->waiting_idr = 1;
  pthread_mutex_unlock(&video->mutex);
  fprintf(stderr, "K2B: input rejected (%d%s); requesting IDR recovery\n",
          result, injected ? ", diagnostic drop" : "");
  return DR_NEED_IDR;
}

DECODER_RENDERER_CALLBACKS decoder_callbacks_k2b = {
  .setup = k2b_setup,
  .stop = k2b_stop,
  .cleanup = k2b_cleanup,
  .submitDecodeUnit = k2b_submit,
  .capabilities = CAPABILITY_DIRECT_SUBMIT,
};
