/* Native integration test: link the unmodified production video/presenter
 * objects. Wrap the display-call boundary to observe pixels/submissions,
 * with optional bounded tracing of the real Cedar API results;
 * all actual decode, import, fences, release and cleanup remain production code.
 * Hash mode reads DMA-BUF pixels and is NOT a throughput measurement.
 */
#define _POSIX_C_SOURCE 200809L
#include "../../src/video/video.h"
#include "../../src/video/k2b/disp_presenter.h"
#include "../../src/video/k2b/cedar_runtime.h"
#include <libavformat/avformat.h>
#include <libavutil/md5.h>
#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <pthread.h>

/* No network connection in this replay executable. */
pthread_t main_thread_id;
void LiRequestIdrFrame(void) { fprintf(stderr, "REPLAY: IDR requested without network\n"); }

static atomic_uint presented;
static atomic_int observation_failed;
static unsigned target;
static int hash_pixels;
static volatile sig_atomic_t interrupted;
static uint64_t first_ns, last_ns;

/* Test-only API tracing; never changes Cedar arguments or return values. */
static const struct k2b_cedar_api *real_api;
static struct k2b_cedar_api trace_api;
static int trace_frames(VideoDecoder *d, int stream)
{
    static unsigned calls;
    int result = real_api->stream_frames(d, stream), saved_errno = errno;
    if (calls++ < 16) fprintf(stderr, "TRACE stream_frames=%d\n", result);
    errno = saved_errno;
    return result;
}
static int trace_request(VideoDecoder *d, int bytes, char **p, int *n,
                         char **wrap, int *wrap_n, int stream)
{
    static unsigned calls;
    int result = real_api->request_stream(d, bytes, p, n, wrap, wrap_n, stream);
    int saved_errno = errno;
    if (calls++ < 16) fprintf(stderr, "TRACE request_stream bytes=%d result=%d\n", bytes, result);
    errno = saved_errno;
    return result;
}
static int trace_decode(VideoDecoder *d, int eos, int key, int drop, int64_t time)
{
    static unsigned calls;
    int result = real_api->decode(d, eos, key, drop, time), saved_errno = errno;
    if (calls++ < 16) fprintf(stderr, "TRACE decode=%d\n", result);
    errno = saved_errno;
    return result;
}
int __real_k2b_cedar_runtime_load(const char *, const struct k2b_cedar_api **);
int __wrap_k2b_cedar_runtime_load(const char *directory, const struct k2b_cedar_api **out)
{
    int result = __real_k2b_cedar_runtime_load(directory, out);
    if (!result && getenv("K2B_REPLAY_TRACE")) {
        real_api = *out;
        trace_api = *real_api;
        trace_api.stream_frames = trace_frames;
        trace_api.request_stream = trace_request;
        trace_api.decode = trace_decode;
        *out = &trace_api;
    }
    return result;
}

static uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}
static void tick(void)
{
    const struct timespec t = {0, 1000000};
    nanosleep(&t, NULL);
}
static void on_signal(int sig) { (void)sig; interrupted = 1; }

static int frame_md5(const struct k2b_frame *f, unsigned char digest[16])
{
    if (k2b_frame_validate(f) < 0) return -1;
    unsigned char *map = mmap(NULL, f->allocation_bytes, PROT_READ,
                              MAP_SHARED, f->fd, 0);
    if (map == MAP_FAILED) return -1;
    struct AVMD5 *md5 = av_md5_alloc();
    if (!md5) { munmap(map, f->allocation_bytes); return -1; }
    struct dma_buf_sync sync = {.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ};
    int result = ioctl(f->fd, DMA_BUF_IOCTL_SYNC, &sync);
    if (!result) {
        av_md5_init(md5);
        for (unsigned y = 0; y < f->height; ++y)
            av_md5_update(md5, map + f->y_offset +
                (f->crop_y + y) * f->stride + f->crop_x, f->width);
        for (unsigned y = 0; y < f->height / 2; ++y)
            av_md5_update(md5, map + f->uv_offset +
                (f->crop_y / 2 + y) * f->stride + f->crop_x, f->width);
        av_md5_final(md5, digest);
        sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
        result = ioctl(f->fd, DMA_BUF_IOCTL_SYNC, &sync);
    }
    av_free(md5);
    if (munmap(map, f->allocation_bytes) < 0) result = -1;
    return result;
}

int __real_k2b_disp_present(struct k2b_disp *, const struct k2b_frame *, uint32_t, int *);
int __wrap_k2b_disp_present(struct k2b_disp *d, const struct k2b_frame *f,
                           uint32_t id, int *fence)
{
    unsigned index = atomic_load(&presented);
    unsigned char digest[16];
    if (hash_pixels && index < target && frame_md5(f, digest) < 0) {
        atomic_store(&observation_failed, 1);
        return -1;
    }
    int result = __real_k2b_disp_present(d, f, id, fence);
    if (result < 0) { atomic_store(&observation_failed, 1); return result; }
    uint64_t at = now_ns();
    if (!index) first_ns = at;
    if (index < target) {
        last_ns = at;
        char hex[33] = {0};
        if (hash_pixels) {
            for (unsigned i = 0; i < sizeof(digest); ++i)
                snprintf(hex + 2 * i, 3, "%02x", digest[i]);
        }
        printf("FRAME %u %llu%s%s\n", index, (unsigned long long)at,
               hash_pixels ? " " : "", hex);
    }
    atomic_store(&presented, index + 1);
    return 0;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    struct sigaction action = {0};
    action.sa_handler = on_signal;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) || sigaction(SIGTERM, &action, NULL))
        return 1;
    if (argc == 2 && !strcmp(argv[1], "--signal-check")) {
        raise(SIGINT);
        raise(SIGINT);
        raise(SIGTERM);
        raise(SIGTERM);
        puts("PASS: repeated stop signals handled without terminating cleanup");
        return interrupted ? 0 : 1;
    }
    if (argc != 4 || (strcmp(argv[3], "hash") && strcmp(argv[3], "pace"))) {
        fprintf(stderr, "usage: %s SAMPLE.h264 FRAMES hash|pace\n", argv[0]);
        return 2;
    }
    char *end;
    unsigned long count = strtoul(argv[2], &end, 10);
    if (!count || count > 600 || *end) return 2;
    target = (unsigned)count;
    hash_pixels = !strcmp(argv[3], "hash");
    AVFormatContext *input = NULL;
    AVPacket *packet = av_packet_alloc(), *first = av_packet_alloc();
    if (!packet || !first || avformat_open_input(&input, argv[1], NULL, NULL) < 0 ||
        avformat_find_stream_info(input, NULL) < 0) return 1;
    int stream = av_find_best_stream(input, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (stream < 0 || input->streams[stream]->codecpar->codec_id != AV_CODEC_ID_H264 ||
        input->streams[stream]->codecpar->width != 1920 ||
        input->streams[stream]->codecpar->height != 1080) return 1;
    video_k2b_configure(COLOR_RANGE_LIMITED);
    if (decoder_callbacks_k2b.setup(VIDEO_FORMAT_H264, 1920, 1080, 60, NULL, 0))
        return 1;
    uint64_t start = now_ns(), deadline = start + 90000000000ULL;
    unsigned sent = 0;
    int error = 0;
    /* One following AU lets the real, non-EOS streaming path release its last
     * picture. At file EOF reuse the first IDR as padding, not as reference data.
     * Input is bounded to four AUs ahead while slow diagnostic hashing runs. */
    while (sent <= target && !interrupted && !atomic_load(&observation_failed)) {
        while ((sent > atomic_load(&presented) + 4 ||
                (!hash_pixels && now_ns() < start + sent * 1000000000ULL / 60)) &&
               !interrupted && now_ns() < deadline) tick();
        if (interrupted || now_ns() >= deadline) { error = 1; break; }
        int read = av_read_frame(input, packet);
        if (read == AVERROR_EOF && sent == target && first->size) {
            read = av_packet_ref(packet, first);
        }
        if (read < 0) { error = 1; break; }
        if (packet->stream_index != stream) { av_packet_unref(packet); continue; }
        if (!sent && av_packet_ref(first, packet) < 0) { error = 1; break; }
        LENTRY entry = {.data = (char *)packet->data, .length = packet->size,
                         .bufferType = BUFFER_TYPE_PICDATA};
        DECODE_UNIT unit = {.frameNumber = (int)sent, .fullLength = packet->size,
            .bufferList = &entry, .presentationTimeUs = sent * 1000000ULL / 60,
            .frameType = packet->flags & AV_PKT_FLAG_KEY ? FRAME_TYPE_IDR : FRAME_TYPE_PFRAME,
            .colorspace = COLORSPACE_REC_709};
        if (decoder_callbacks_k2b.submitDecodeUnit(&unit) != DR_OK) {
            error = 1; av_packet_unref(packet); break;
        }
        ++sent;
        av_packet_unref(packet);
    }
    while (atomic_load(&presented) < target && !interrupted &&
           !atomic_load(&observation_failed) && now_ns() < deadline) tick();
    decoder_callbacks_k2b.stop();
    decoder_callbacks_k2b.cleanup();
    unsigned displayed = atomic_load(&presented);
    double seconds = (last_ns - first_ns) / 1e9;
    fprintf(stderr, "REPLAY mode=%s sent=%u observed=%u target=%u span_s=%.6f submission_fps=%.3f\n",
        argv[3], sent, displayed, target, seconds,
        seconds > 0 ? (target - 1) / seconds : 0);
    av_packet_free(&first);
    av_packet_free(&packet);
    avformat_close_input(&input);
    return error || interrupted || atomic_load(&observation_failed) || displayed < target;
}
