/* Exercise the actual worker with unavailable hardware replaced at its API
 * boundary. This proves scheduling/ownership, not physical fence semantics. */
#define nanosleep test_nanosleep
#include "../../src/video/k2b/video.c"
#undef nanosleep

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL: %s\n", #x); exit(1); } } while (0)
static struct k2b_video *fixture;
static int ticks, decodes, requests, returns, presents;
static VideoPicture output;
static char compressed[64];
static struct ScMemOpsS memops;

int test_nanosleep(const struct timespec *delay, struct timespec *remaining)
{
  (void)delay; (void)remaining;
  if (++ticks == 3) fixture->stop = 1;
  return 0;
}
static VideoDecoder *create(void) { return (VideoDecoder *)&output; }
static void destroy(VideoDecoder *d) { (void)d; }
static int initialize(VideoDecoder *d, VideoStreamInfo *s, VConfig *c)
{ (void)d; (void)s; (void)c; return 0; }
static void reset(VideoDecoder *d) { (void)d; }
static int decode(VideoDecoder *d, int a, int b, int c, int64_t t)
{ (void)d; (void)a; (void)b; (void)c; (void)t; decodes++; return VDECODE_RESULT_FRAME_DECODED; }
static int request_stream(VideoDecoder *d, int n, char **a, int *an, char **b, int *bn, int i)
{ (void)d; (void)n; (void)i; *a = compressed; *an = sizeof(compressed); *b = NULL; *bn = 0; return 0; }
static int submit_stream(VideoDecoder *d, VideoStreamDataInfo *s, int i)
{ (void)d; (void)s; (void)i; return 0; }
static int stream_frames(VideoDecoder *d, int i) { (void)d; (void)i; return 0; }
static VideoPicture *request_picture(VideoDecoder *d, int i)
{ (void)d; (void)i; requests++; return requests == 1 ? &output : NULL; }
static int return_picture(VideoDecoder *d, VideoPicture *p)
{ (void)d; CHECK(p == &output); returns++; return 0; }
static struct ScMemOpsS *mem_ops(void) { return &memops; }
static int memory_begin(void) { return 0; }
static int memory_end(void) { return 0; }
static int memory_status(struct k2b_cedar_memory_status *s)
{ memset(s, 0, sizeof(*s)); return 0; }
static const struct k2b_cedar_api api = {
  .create = create, .destroy = destroy, .initialize = initialize, .reset = reset,
  .decode = decode, .request_stream = request_stream, .submit_stream = submit_stream,
  .stream_frames = stream_frames, .request_picture = request_picture,
  .return_picture = return_picture, .mem_ops = mem_ops, .memory_begin = memory_begin,
  .memory_end = memory_end, .memory_status = memory_status,
};
int k2b_cedar_runtime_load(const char *directory, const struct k2b_cedar_api **out)
{ (void)directory; *out = &api; return 0; }
int k2b_disp_open(struct k2b_disp **out)
{ *out = (struct k2b_disp *)&output; return 0; }
int k2b_disp_present(struct k2b_disp *d, const struct k2b_frame *f, uint32_t id, int *fd)
{ (void)d; (void)f; (void)id; (void)fd; presents++; return -1; }
int k2b_disp_retire(struct k2b_disp *d) { (void)d; return 0; }
void k2b_disp_close(struct k2b_disp *d) { (void)d; }

int main(void)
{
  struct k2b_video v = {0};
  fixture = &v;
  CHECK(pthread_mutex_init(&v.mutex, NULL) == 0);
  CHECK(pthread_cond_init(&v.ready_cond, NULL) == 0);
  CHECK(k2b_input_queue_create(&v.queue, 2, 64) == K2B_QUEUE_OK);
  unsigned char bytes[] = {0, 0, 1, 0x65};
  LENTRY entry = {.data = (char *)bytes, .length = sizeof(bytes), .bufferType = BUFFER_TYPE_PICDATA};
  DECODE_UNIT unit = {.bufferList = &entry, .fullLength = sizeof(bytes),
    .frameType = FRAME_TYPE_IDR, .colorspace = COLORSPACE_REC_709};
  CHECK(k2b_input_queue_push(v.queue, &unit) == K2B_QUEUE_OK);
  /* Two display-held slots whose fences do not permit another commit yet. */
  v.held_count = 2;
  v.held[0].release_fd = v.held[1].release_fd = -1;
  CHECK(setenv("K2B_CEDAR_RUNTIME_DIR", "/test-only", 1) == 0);
  video_worker(&v);
  CHECK(decodes == 1); /* Prepare one frame while waiting, then stop decoding. */
  CHECK(requests == 1);
  CHECK(presents == 0); /* Never bypass display retirement. */
  CHECK(returns == 1); /* Pending, never-displayed frame is returned at stop. */
  CHECK(v.held_count == 0);
  CHECK(!v.failed);
  CHECK(k2b_input_queue_stop(v.queue) == K2B_QUEUE_OK);
  CHECK(k2b_input_queue_destroy(&v.queue) == K2B_QUEUE_OK);
  pthread_cond_destroy(&v.ready_cond);
  pthread_mutex_destroy(&v.mutex);
  puts("video predecode scheduling: PASS");
  return 0;
}
