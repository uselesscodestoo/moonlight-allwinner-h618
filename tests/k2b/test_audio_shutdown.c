#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <alsa/asoundlib.h>
#include <opus_multistream.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL: %s\n", #x); exit(1); } } while (0)
static int writes, waits, drops, drains;
static int normal_writes;
static int fake_decode(OpusMSDecoder *d, const unsigned char *b, opus_int32 n,
                       opus_int16 *p, int count, int fec)
{ (void)d; (void)b; (void)n; (void)p; (void)count; (void)fec; return 480; }
static snd_pcm_sframes_t fake_write(snd_pcm_t *h, const void *p, snd_pcm_uframes_t n)
{ (void)h; (void)p; writes++; return normal_writes ? (snd_pcm_sframes_t)(n > 100 ? 100 : n) : -EAGAIN; }
static int fake_wait(snd_pcm_t *h, int ms)
{ (void)h; (void)ms; CHECK(++waits <= 20); return 1; }
static int fake_drop(snd_pcm_t *h) { (void)h; drops++; return 0; }
static int fake_drain(snd_pcm_t *h) { (void)h; drains++; return 0; }
static int fake_close(snd_pcm_t *h) { (void)h; return 0; }
static int fake_clock(clockid_t id, struct timespec *ts)
{ (void)id; static long ms; ms += 5; ts->tv_sec = ms / 1000; ts->tv_nsec = ms % 1000 * 1000000; return 0; }
#define opus_multistream_decode fake_decode
#define snd_pcm_writei fake_write
#define snd_pcm_wait fake_wait
#define snd_pcm_drop fake_drop
#define snd_pcm_drain fake_drain
#define snd_pcm_close fake_close
#define clock_gettime fake_clock
#include "../../src/audio/alsa.c"

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    blockingPlayback = true;
    handle = (snd_pcm_t *)1;
    channelCount = 2;
    samplesPerFrame = 480;
    pcmBuffer = calloc(960, sizeof(short));
    if (!strcmp(argv[1], "normal")) {
        normal_writes = 1;
        alsa_renderer_decode_and_play_sample(NULL, 0);
        CHECK(framesWritten == 480 && writes == 5);
    } else if (!strcmp(argv[1], "wait")) {
        alsa_renderer_decode_and_play_sample(NULL, 0);
        CHECK(writes > 0 && waits <= 20);
    } else if (!strcmp(argv[1], "stop")) {
        CHECK(audio_callbacks_alsa_k2b.stop != NULL);
        audio_callbacks_alsa_k2b.stop();
        alsa_renderer_decode_and_play_sample(NULL, 0);
        CHECK(writes == 0);
    } else if (!strcmp(argv[1], "cleanup")) {
        alsa_renderer_cleanup();
        CHECK(drops == 1 && drains == 0);
    } else return 2;
    free(pcmBuffer);
    (void)fake_drop; (void)fake_clock;
    puts("K2B bounded audio/stop: PASS");
    return 0;
}
