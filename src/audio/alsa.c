/*
 * This file is part of Moonlight Embedded.
 *
 * Copyright (C) 2015-2017 Iwan Timmer
 *
 * Moonlight is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * Moonlight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Moonlight; if not, see <http://www.gnu.org/licenses/>.
 */

#include "audio.h"

#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include <time.h>

#include <opus_multistream.h>
#include <alsa/asoundlib.h>

#define CHECK_RETURN(f) if ((rc = f) < 0) { printf("Alsa error code %d\n", rc); return -1; }

static snd_pcm_t *handle;
static OpusMSDecoder* decoder;
static short* pcmBuffer;
static int samplesPerFrame;
static int channelCount;
static bool blockingPlayback;
static unsigned long long framesWritten;
static unsigned int audioRecoveries;
static atomic_bool playbackStopped;

static uint64_t audio_now_ms(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static int alsa_renderer_init_common(int audioConfiguration, POPUS_MULTISTREAM_CONFIGURATION opusConfig, void* context, int arFlags, bool blocking) {
  int rc;
  unsigned char alsaMapping[AUDIO_CONFIGURATION_MAX_CHANNEL_COUNT];
  blockingPlayback = blocking;
  channelCount = opusConfig->channelCount;
  framesWritten = 0;
  audioRecoveries = 0;
  atomic_store(&playbackStopped, false);

  /* The supplied mapping array has order: FL-FR-C-LFE-RL-RR-SL-SR
   * ALSA expects the order: FL-FR-RL-RR-C-LFE-SL-SR
   * We need copy the mapping locally and swap the channels around.
   */
  memcpy(alsaMapping, opusConfig->mapping, sizeof(alsaMapping));
  if (opusConfig->channelCount >= 6) {
    alsaMapping[2] = opusConfig->mapping[4];
    alsaMapping[3] = opusConfig->mapping[5];
    alsaMapping[4] = opusConfig->mapping[2];
    alsaMapping[5] = opusConfig->mapping[3];
  }

  samplesPerFrame = opusConfig->samplesPerFrame;
  pcmBuffer = malloc(sizeof(short) * opusConfig->channelCount * samplesPerFrame);
  if (pcmBuffer == NULL)
    return -1;

  decoder = opus_multistream_decoder_create(opusConfig->sampleRate, opusConfig->channelCount, opusConfig->streams, opusConfig->coupledStreams, alsaMapping, &rc);

  snd_pcm_hw_params_t *hw_params;
  snd_pcm_sw_params_t *sw_params;
  snd_pcm_uframes_t period_size = (opusConfig->sampleRate * 20) / 1000; // 20 ms period
  snd_pcm_uframes_t buffer_size = 3 * period_size; // 60 ms buffer
  unsigned int sampleRate = opusConfig->sampleRate;

  char* audio_device = (char*) context;
  if (audio_device == NULL)
    audio_device = "sysdefault";

  /* Open PCM device for playback. */
  CHECK_RETURN(snd_pcm_open(&handle, audio_device, SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK))

  /* Set hardware parameters */
  CHECK_RETURN(snd_pcm_hw_params_malloc(&hw_params));
  CHECK_RETURN(snd_pcm_hw_params_any(handle, hw_params));
  CHECK_RETURN(snd_pcm_hw_params_set_access(handle, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED));
  CHECK_RETURN(snd_pcm_hw_params_set_format(handle, hw_params, SND_PCM_FORMAT_S16_LE));
  CHECK_RETURN(snd_pcm_hw_params_set_rate_near(handle, hw_params, &sampleRate, NULL));
  CHECK_RETURN(snd_pcm_hw_params_set_channels(handle, hw_params, opusConfig->channelCount));
  CHECK_RETURN(snd_pcm_hw_params_set_period_size_near(handle, hw_params, &period_size, NULL));
  CHECK_RETURN(snd_pcm_hw_params_set_buffer_size_near(handle, hw_params, &buffer_size));
  CHECK_RETURN(snd_pcm_hw_params(handle, hw_params));
  snd_pcm_hw_params_free(hw_params);

  /* Set software parameters */
  CHECK_RETURN(snd_pcm_sw_params_malloc(&sw_params));
  CHECK_RETURN(snd_pcm_sw_params_current(handle, sw_params));
  CHECK_RETURN(snd_pcm_sw_params_set_avail_min(handle, sw_params, period_size));
  CHECK_RETURN(snd_pcm_sw_params_set_start_threshold(handle, sw_params, period_size));
  CHECK_RETURN(snd_pcm_sw_params(handle, sw_params));
  snd_pcm_sw_params_free(sw_params);

  CHECK_RETURN(snd_pcm_prepare(handle));

  return 0;
}

static int alsa_renderer_init(int audioConfiguration, POPUS_MULTISTREAM_CONFIGURATION opusConfig, void* context, int arFlags) {
  return alsa_renderer_init_common(audioConfiguration, opusConfig, context, arFlags, false);
}

#ifdef HAVE_K2B
static int alsa_k2b_renderer_init(int audioConfiguration, POPUS_MULTISTREAM_CONFIGURATION opusConfig, void* context, int arFlags) {
  return alsa_renderer_init_common(audioConfiguration, opusConfig, context, arFlags, true);
}
static void alsa_k2b_renderer_stop(void) {
  atomic_store(&playbackStopped, true);
}
#endif

static void alsa_renderer_cleanup() {
  if (blockingPlayback)
    fprintf(stderr, "K2B ALSA: frames_written=%llu recoveries=%u\n", framesWritten, audioRecoveries);
  if (decoder != NULL) {
    opus_multistream_decoder_destroy(decoder);
    decoder = NULL;
  }

  if (handle != NULL) {
    if (blockingPlayback) snd_pcm_drop(handle);
    else snd_pcm_drain(handle);
    snd_pcm_close(handle);
    handle = NULL;
  }

  if (pcmBuffer != NULL) {
    free(pcmBuffer);
    pcmBuffer = NULL;
  }
}

static void alsa_renderer_decode_and_play_sample(char* data, int length) {
  if (blockingPlayback && atomic_load(&playbackStopped)) return;
  int decodeLen = opus_multistream_decode(decoder, data, length, pcmBuffer, samplesPerFrame, 0);
  if (decodeLen > 0) {
    if (blockingPlayback) {
      int offset = 0;
      uint64_t deadline = audio_now_ms() + 100;
      while (offset < decodeLen && !atomic_load(&playbackStopped) &&
             audio_now_ms() < deadline) {
        int written = snd_pcm_writei(handle, pcmBuffer + offset * channelCount, decodeLen - offset);
        if (written > 0) {
          offset += written;
          framesWritten += written;
          continue;
        }
        if (written == 0 || written == -EAGAIN) {
          snd_pcm_wait(handle, 10);
          continue;
        }
        audioRecoveries++;
        /* snd_pcm_recover may repeatedly sleep on a suspended device. Keep
         * this path bounded and observable by the stop callback instead. */
        if (written == -EINTR) continue;
        if ((written != -EPIPE && written != -ESTRPIPE) || snd_pcm_prepare(handle) < 0) {
          fprintf(stderr, "K2B ALSA: playback error %d\n", written);
          return;
        }
      }
      return;
    }
    int rc = snd_pcm_writei(handle, pcmBuffer, decodeLen);
    if (rc < 0) {
      rc = snd_pcm_recover(handle, rc, 0);
      if (rc == 0)
        rc = snd_pcm_writei(handle, pcmBuffer, decodeLen);
    }

    if (rc<0)
      printf("Alsa error from writei: %d\n", rc);
    else if (decodeLen != rc)
      printf("Alsa shortm write, write %d frames\n", rc);
  } else if (decodeLen < 0) {
    printf("Opus error from decode: %d\n", decodeLen);
  }
}

AUDIO_RENDERER_CALLBACKS audio_callbacks_alsa = {
  .init = alsa_renderer_init,
  .cleanup = alsa_renderer_cleanup,
  .decodeAndPlaySample = alsa_renderer_decode_and_play_sample,
  .capabilities = CAPABILITY_DIRECT_SUBMIT | CAPABILITY_SUPPORTS_ARBITRARY_AUDIO_DURATION,
};

#ifdef HAVE_K2B
/* Bounded playback waiting belongs on common-c's audio decoder thread, not the
 * network receive thread. Preserve other platforms' existing ALSA path. */
AUDIO_RENDERER_CALLBACKS audio_callbacks_alsa_k2b = {
  .init = alsa_k2b_renderer_init,
  .stop = alsa_k2b_renderer_stop,
  .cleanup = alsa_renderer_cleanup,
  .decodeAndPlaySample = alsa_renderer_decode_and_play_sample,
  .capabilities = CAPABILITY_SUPPORTS_ARBITRARY_AUDIO_DURATION,
};
#endif
