#include "aac_decoder.h"
#include "audio_format_log.h"
#include "audio_out_sample.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "airplay_log.h"
#include "sdkconfig.h"
#include "aacdecoder_lib.h"

#if defined(CONFIG_AIRPLAY_MULTICHANNEL) && CONFIG_AIRPLAY_MULTICHANNEL
#define AAC_MULTICHANNEL 1
#else
#define AAC_MULTICHANNEL 0
#endif

#define ADTS_HEADER_LEN AAC_DECODER_INPUT_HEADROOM
#define AAC_INPUT_MAX   8192U
/* S32 scratch for one AAC-LC access unit (2048
 * leaves room in case the decoder hands out more than 1024 frames). */
#define MC_MAX_CHANNELS 8
#define MC_MAX_FRAMES   2048U
/* CPU-time statistics are logged once per this many decoded frames
 * (~10.7 s at 48 kHz / 1024). */
#define MC_TIMING_EVERY 500U

struct aac_decoder {
  aac_decoder_config_t config;
  HANDLE_AACDECODER fdk;
  LONG *mc_pcm; /* FDK signed32 before 16-bit quantization; never INT_PCM. */
  uint32_t decode_errors;
  uint32_t mc_decodes;
#if AAC_MULTICHANNEL
  bool mc_layout_known;
  int mc_lfe_index; /* -1 if the stream has no LFE */
  int mc_channels;
  /* Decoded channel index of each aac_speaker_t (-1 = not in the stream),
   * and FDK's label of each decoded channel. */
  int8_t mc_speaker_channel[AAC_SPEAKER_COUNT];
  AUDIO_CHANNEL_TYPE mc_type[MC_MAX_CHANNELS];
  UCHAR mc_index[MC_MAX_CHANNELS];
  char mc_label[MC_MAX_CHANNELS][16];
  int mc_applied_speaker; /* speaker the synth mask is set for; -2 = none */
  int mc_play_channel;    /* decoded channel played, -1 = mono mix */
  int64_t mc_time_sum_us;
  int64_t mc_time_max_us;
  uint32_t mc_time_frames;
  /* Per stage (parse / synthesis / output) over the same window, and how
   * many frames took FDK's single-channel output (local fdk-aac patch). */
  int64_t mc_stage_sum_us[3];
  int64_t mc_stage_max_us[3];
  uint32_t mc_stage_frames;
  uint32_t mc_fast_frames;
#endif
};

static const char *TAG = "aac_dec_v1";

static int adts_frequency_index(int sample_rate) {
  static const int rates[] = {96000, 88200, 64000, 48000, 44100, 32000,
                              24000, 22050, 16000, 12000, 11025, 8000, 7350};
  for (int i = 0; i < (int)(sizeof(rates) / sizeof(rates[0])); ++i) {
    if (rates[i] == sample_rate) {
      return i;
    }
  }
  return 4; // AirPlay 2 path is 44.1 kHz in this project.
}

static bool has_adts(const uint8_t *data, size_t len) {
  return len >= 2U && data[0] == 0xffU && (data[1] & 0xf0U) == 0xf0U;
}

static void make_adts(uint8_t *h, size_t payload_len, int sample_rate,
                      int channels) {
  const int profile = 2; // AAC LC
  const int freq_idx = adts_frequency_index(sample_rate);
  /* ADTS channel_configuration: 1..6 = that many channels (6 = 5.1),
   * 7 = 8 channels (7.1). */
  const int chan_cfg = channels == 8 ? 7 : (channels > 0 ? channels : 2);
  const int packet_len = (int)(payload_len + ADTS_HEADER_LEN);

  h[0] = 0xff;
  h[1] = 0xf1;
  h[2] = (uint8_t)(((profile - 1) << 6) | (freq_idx << 2) | (chan_cfg >> 2));
  h[3] = (uint8_t)(((chan_cfg & 3) << 6) | (packet_len >> 11));
  h[4] = (uint8_t)((packet_len & 0x7ff) >> 3);
  h[5] = (uint8_t)(((packet_len & 7) << 5) | 0x1f);
  h[6] = 0xfc;
}

/* Crash breadcrumbs: the last FDK step and frame count survive a panic
 * reboot in RTC memory, so the next boot can say where it died even when
 * only the web log (not the UART panic dump) is visible. */
enum {
  MC_STAGE_IDLE = 0,
  MC_STAGE_ALLOC,
  MC_STAGE_OPEN,
  MC_STAGE_SETPARAM,
  MC_STAGE_FILL,
  MC_STAGE_DECODE,
  MC_STAGE_POST,
};
#define MC_TRACE_MAGIC 0x4D43A0A0U
#if AAC_MULTICHANNEL
static RTC_NOINIT_ATTR uint32_t s_mc_trace_magic;
static RTC_NOINIT_ATTR uint32_t s_mc_trace_stage;
static RTC_NOINIT_ATTR uint32_t s_mc_trace_frames;

static inline void mc_trace(uint32_t stage, uint32_t frames) {
  s_mc_trace_stage = stage;
  s_mc_trace_frames = frames;
  s_mc_trace_magic = MC_TRACE_MAGIC;
}
#else
#define mc_trace(stage, frames) ((void)0)
#endif

#if AAC_MULTICHANNEL
/* Selected speaker (aac_speaker_t), written by the web UI / boot code and
 * read by the decode task once per frame. */
static int s_mc_speaker =
#ifdef CONFIG_AIRPLAY_MULTICHANNEL_DEFAULT_SPEAKER
    CONFIG_AIRPLAY_MULTICHANNEL_DEFAULT_SPEAKER;
#else
    AAC_SPEAKER_MIX;
#endif

/* What the running decoder plays, for the web UI. */
static portMUX_TYPE s_mc_status_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_mc_status_active;
static int s_mc_status_channel;
static int s_mc_status_channels;
static char s_mc_status_label[16];

static void mc_publish_status(bool active, int channel, int channels,
                              const char *label) {
  taskENTER_CRITICAL(&s_mc_status_mux);
  s_mc_status_active = active;
  s_mc_status_channel = channel;
  s_mc_status_channels = channels;
  strlcpy(s_mc_status_label, label ? label : "", sizeof(s_mc_status_label));
  taskEXIT_CRITICAL(&s_mc_status_mux);
}

/* Clock for FDK's per-stage timestamps (local fdk-aac patch). */
static INT64 mc_clock_us(void) { return (INT64)esp_timer_get_time(); }
#endif

static bool open_fdk_decoder(aac_decoder_t *d) {
  mc_trace(MC_STAGE_OPEN, 0);
  d->fdk = aacDecoder_Open(TT_MP4_ADTS, 1);
  if (!d->fdk) {
    ESP_LOGE(TAG, "FDK aacDecoder_Open failed");
    return false;
  }
  /* Keep every encoded channel (no FDK downmix) in WAV order, and no
   * limiter: DecodeFrame32 requires it disabled. Preserve FDK's existing
   * DRC defaults and scaling; no extra gain or source-depth assumption. */
  mc_trace(MC_STAGE_SETPARAM, 0);
  const AAC_DECODER_ERROR e1 =
      aacDecoder_SetParam(d->fdk, AAC_PCM_MAX_OUTPUT_CHANNELS, MC_MAX_CHANNELS);
  const AAC_DECODER_ERROR e2 =
      aacDecoder_SetParam(d->fdk, AAC_PCM_OUTPUT_CHANNEL_MAPPING, 1);
  const AAC_DECODER_ERROR e3 =
      aacDecoder_SetParam(d->fdk, AAC_PCM_LIMITER_ENABLE, 0);
  /* RTP already timestamps each AU. FDK's default interpolation adds one
   * unaccounted frame (1024 samples); noise substitution has no extra delay. */
  const AAC_DECODER_ERROR e4 =
      aacDecoder_SetParam(d->fdk, AAC_CONCEAL_METHOD, 1);
  /* The first frame synthesizes every channel; once the layout is known the
   * synth mask is narrowed to the selected speaker (mc_apply_speaker). */
  if (e1 != AAC_DEC_OK || e2 != AAC_DEC_OK || e3 != AAC_DEC_OK || e4 != AAC_DEC_OK) {
    ESP_LOGE(TAG, "FDK S32 setup failed (params 0x%x/0x%x/0x%x/0x%x)",
             e1, e2, e3, e4);
    return false;
  }
#if AAC_MULTICHANNEL
  d->mc_applied_speaker = -2;
  d->mc_play_channel = -1;
  d->mc_lfe_index = -1;
  if (d->config.channels > 2) aacDecoder_SetTimeHook(d->fdk, mc_clock_us);
#endif
  mc_trace(MC_STAGE_IDLE, 0);
  return true;
}

/* EXPERIMENT: one decoded channel on both stereo slots. */
static void mc_pick_channel(const LONG *in, int channels, int pick,
                            size_t frames, float *out) {
  for (size_t f = 0; f < frames; ++f) {
    const float v = audio_pcm32_to_float(in[f * (size_t)channels + (size_t)pick]);
    out[2 * f] = v;
    out[2 * f + 1] = v;
  }
}

#if AAC_MULTICHANNEL
/* EXPERIMENT: fold every channel except LFE into mono on both slots. */
static void mc_downmix_to_mono(const LONG *in, int channels, int lfe,
                               size_t frames, float *out) {
  const int used = lfe >= 0 ? channels - 1 : channels;
  const float gain = AUDIO_PCM32_TO_FLOAT / (float)used;
  for (size_t f = 0; f < frames; ++f) {
    const LONG *s = in + f * (size_t)channels;
    /* At most eight S32 values: exact sum without overflow or losing a
     * quiet channel when louder channels cancel. Convert only once. */
    int64_t sum = 0;
    for (int c = 0; c < channels; ++c) {
      if (c != lfe) sum += s[c];
    }
    const float m = (float)sum * gain;
    out[2 * f] = m;
    out[2 * f + 1] = m;
  }
}

static const char *mc_channel_type_name(AUDIO_CHANNEL_TYPE t) {
  switch (t) {
    case ACT_FRONT: return "front";
    case ACT_SIDE: return "side";
    case ACT_BACK: return "back";
    case ACT_LFE: return "lfe";
    case ACT_FRONT_TOP: return "front-top";
    case ACT_SIDE_TOP: return "side-top";
    case ACT_BACK_TOP: return "back-top";
    default: return "none";
  }
}

static int mc_find(const CStreamInfo *si, AUDIO_CHANNEL_TYPE type, int index) {
  for (int c = 0; c < si->numChannels && c < MC_MAX_CHANNELS; ++c) {
    if (si->pChannelType[c] == type && si->pChannelIndices[c] == index) return c;
  }
  return -1;
}

static int mc_count(const CStreamInfo *si, AUDIO_CHANNEL_TYPE type) {
  int n = 0;
  for (int c = 0; c < si->numChannels && c < MC_MAX_CHANNELS; ++c) {
    if (si->pChannelType[c] == type) ++n;
  }
  return n;
}

/* Once per decoder: map every speaker name to a decoded channel.
 *
 * FDK labels channels by MPEG position (front #0..n, side, back, lfe). 5.1
 * (configuration 6) is unambiguous: front C/L/R, back = surround pair. For
 * 7.1, Apple TV sends configuration 7, which FDK (following ISO) reports as
 * five fronts + two backs: front #0 = C, #1/#2 and #3/#4 = two pairs. ISO
 * calls #1/#2 "left/right centre" and #3/#4 L/R; Apple's 7.1 order (C L R
 * Ls Rs Lrs Rrs LFE) makes #1/#2 L/R, #3/#4 the side pair and the backs the
 * rear pair. We follow Apple: in the first test the #1/#2 pair carried the
 * most energy of an upmixed stereo song, as main L/R should. */
static void mc_build_speaker_map(aac_decoder_t *d, const CStreamInfo *si) {
  int8_t *m = d->mc_speaker_channel;
  for (int s = 0; s < AAC_SPEAKER_COUNT; ++s) m[s] = -1;
  if (!si->pChannelType || !si->pChannelIndices) {
    d->mc_layout_known = true;
    return;
  }
  const int fronts = mc_count(si, ACT_FRONT);
  const int sides = mc_count(si, ACT_SIDE);
  const int backs = mc_count(si, ACT_BACK);
  const int lr = (fronts & 1) ? 1 : 0; /* odd count: front #0 is the centre */

  if (fronts & 1) m[AAC_SPEAKER_C] = (int8_t)mc_find(si, ACT_FRONT, 0);
  m[AAC_SPEAKER_L] = (int8_t)mc_find(si, ACT_FRONT, lr);
  m[AAC_SPEAKER_R] = (int8_t)mc_find(si, ACT_FRONT, lr + 1);
  m[AAC_SPEAKER_LFE] = (int8_t)mc_find(si, ACT_LFE, 0);
  bool rear_pair = false;
  if (sides >= 2) {
    m[AAC_SPEAKER_LS] = (int8_t)mc_find(si, ACT_SIDE, 0);
    m[AAC_SPEAKER_RS] = (int8_t)mc_find(si, ACT_SIDE, 1);
    rear_pair = backs >= 2;
  } else if (fronts >= lr + 4) { /* Apple 7.1 in configuration 7 */
    m[AAC_SPEAKER_LS] = (int8_t)mc_find(si, ACT_FRONT, lr + 2);
    m[AAC_SPEAKER_RS] = (int8_t)mc_find(si, ACT_FRONT, lr + 3);
    rear_pair = backs >= 2;
  } else if (backs >= 2) { /* 5.1: the backs are the surround pair */
    m[AAC_SPEAKER_LS] = (int8_t)mc_find(si, ACT_BACK, 0);
    m[AAC_SPEAKER_RS] = (int8_t)mc_find(si, ACT_BACK, 1);
  }
  if (rear_pair) {
    m[AAC_SPEAKER_LRS] = (int8_t)mc_find(si, ACT_BACK, 0);
    m[AAC_SPEAKER_RRS] = (int8_t)mc_find(si, ACT_BACK, 1);
  } else {
    m[AAC_SPEAKER_LRS] = m[AAC_SPEAKER_LS];
    m[AAC_SPEAKER_RRS] = m[AAC_SPEAKER_RS];
  }
  d->mc_lfe_index = m[AAC_SPEAKER_LFE];
  d->mc_channels = si->numChannels;

  for (int c = 0; c < si->numChannels && c < MC_MAX_CHANNELS; ++c) {
    d->mc_type[c] = si->pChannelType[c];
    d->mc_index[c] = si->pChannelIndices[c];
    snprintf(d->mc_label[c], sizeof(d->mc_label[c]), "%s #%u",
             mc_channel_type_name(si->pChannelType[c]),
             si->pChannelIndices[c]);
    const char *speaker = "-";
    for (int s = 0; s < AAC_SPEAKER_COUNT; ++s) {
      /* First match wins, so a 5.1 side pair is named Side, not Rear. */
      if (m[s] == c) {
        speaker = aac_speaker_name(s);
        break;
      }
    }
    ESP_LOGW(TAG, "MC: decoded channel %d = %s -> %s", c, d->mc_label[c],
             speaker);
  }
  ESP_LOGW(TAG, "MC: stream sr=%d ch=%d frame=%d channelConfig=%d aacCh=%d",
           si->sampleRate, si->numChannels, si->frameSize, si->channelConfig,
           si->aacNumChannels);
  d->mc_layout_known = true;
}

/* Point FDK's synthesis (local fdk-aac patch) at the selected speaker, so it
 * runs the filterbank for that channel only: all eight do not fit one frame.
 * The channel is named by its label (type + index), not its number: FDK
 * reorders channels after synthesis, and only the label follows the data.
 * Called before every decode; acts only when the selection changed. A
 * channel switched on mid-stream has stale overlap for one frame. */
static void mc_apply_speaker(aac_decoder_t *d) {
  if (!d->mc_layout_known || d->mc_channels <= 2) return;
  const int speaker = __atomic_load_n(&s_mc_speaker, __ATOMIC_RELAXED);
  if (speaker == d->mc_applied_speaker) return;
  const int channel =
      (speaker >= 0 && speaker < AAC_SPEAKER_COUNT) ? d->mc_speaker_channel[speaker] : -1;
  if (channel >= 0) {
    aacDecoder_SetSynthChannel(d->fdk, d->mc_type[channel], d->mc_index[channel]);
  } else {
    aacDecoder_SetSynthChannel(d->fdk, ACT_NONE, 0);
  }
  d->mc_applied_speaker = speaker;
  d->mc_play_channel = channel;
  audio_log_surround_playback(TAG, d->config.sample_rate, d->mc_channels,
                              aac_speaker_name(speaker), channel);
  mc_publish_status(true, channel, d->mc_channels,
                    channel >= 0 ? d->mc_label[channel] : "mix");
}

/* At the start of a stream and then every MC_TIMING_EVERY frames. In the
 * single-channel fast path only the played channel is in mc_pcm (mono). */
static void mc_log_peaks(aac_decoder_t *d, int ch, size_t frames, bool fast) {
  const uint32_t n = d->mc_decodes;
  if (n > 5 && (n % MC_TIMING_EVERY) != 0) return;
  if (fast) {
    uint32_t peak = 0;
    for (size_t f = 0; f < frames; ++f) {
      const int64_t sample = d->mc_pcm[f];
      const uint32_t v = (uint32_t)(sample < 0 ? -sample : sample);
      if (v > peak) peak = v;
    }
    ESP_LOGW(TAG, "MC #%lu: %u frames, fast path: channel %d only, S32 peak %lu",
             (unsigned long)n, (unsigned)frames, d->mc_play_channel,
             (unsigned long)peak);
    return;
  }
  char peaks[96];
  size_t pos = 0;
  peaks[0] = '\0';
  uint32_t played_peak = 0, other_peak = 0;
  int other_channel = -1;
  for (int c = 0; c < ch && c < MC_MAX_CHANNELS; ++c) {
    uint32_t peak = 0;
    for (size_t f = 0; f < frames; ++f) {
      const int64_t sample = d->mc_pcm[f * (size_t)ch + (size_t)c];
      const uint32_t v = (uint32_t)(sample < 0 ? -sample : sample);
      if (v > peak) peak = v;
    }
    if (c == d->mc_play_channel) {
      played_peak = peak;
    } else if (peak > other_peak) {
      other_peak = peak;
      other_channel = c;
    }
    if (pos < sizeof(peaks))
      pos += (size_t)snprintf(peaks + pos, sizeof(peaks) - pos, "%s%lu",
                              c ? "," : "", (unsigned long)peak);
  }
  ESP_LOGW(TAG, "MC #%lu: %u frames x %d ch, S32 peaks[%s]", (unsigned long)n,
           (unsigned)frames, ch, peaks);
  /* Full output with one channel synthesized: the others are stale planes
   * FDK no longer clears, so only flag a silent played channel when every
   * channel is synthesized (mix) or the played one is plainly missing. */
  if (d->mc_play_channel >= 0 && played_peak == 0 && other_peak > 0 &&
      d->mc_applied_speaker == AAC_SPEAKER_MIX)
    ESP_LOGE(TAG, "MC: played channel %d is silent but channel %d has signal",
             d->mc_play_channel, other_channel);
}

static void mc_account_stage(aac_decoder_t *d, int stage, int64_t us) {
  d->mc_stage_sum_us[stage] += us;
  if (us > d->mc_stage_max_us[stage]) d->mc_stage_max_us[stage] = us;
}

/* Every MC_TIMING_EVERY frames: total decode time per frame against the
 * frame budget, split into parse (bitstream + spectra of all channels),
 * synthesis (concealment/DRC/IMDCT of the played channel) and output
 * (channel reorder + PCM conversion). */
static void mc_account_time(aac_decoder_t *d, int64_t us, int frame_size,
                            int sample_rate) {
  d->mc_time_sum_us += us;
  if (us > d->mc_time_max_us) d->mc_time_max_us = us;
  if (++d->mc_time_frames < MC_TIMING_EVERY) return;
  const int64_t budget_us =
      sample_rate > 0 ? (int64_t)frame_size * 1000000LL / sample_rate : 0;
  const int64_t avg_us = d->mc_time_sum_us / (int64_t)d->mc_time_frames;
  ESP_LOGW(TAG, "MC CPU: FDK decode avg %lld us, max %lld us per frame over %lu "
                "frames; frame budget %lld us -> avg %lld%% of one core; "
                "decode task stack never below %u B free; PSRAM free %u KiB "
                "(min %u KiB)",
           (long long)avg_us, (long long)d->mc_time_max_us,
           (unsigned long)d->mc_time_frames, (long long)budget_us,
           budget_us > 0 ? (long long)(avg_us * 100 / budget_us) : -1LL,
           (unsigned)uxTaskGetStackHighWaterMark(NULL),
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024U),
           (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM) /
                      1024U));
  if (d->mc_stage_frames > 0) {
    const int64_t n = d->mc_stage_frames;
    ESP_LOGW(TAG, "MC CPU stages (avg/max us): parse %lld/%lld, synthesis "
                  "%lld/%lld, output %lld/%lld; fast single-channel output "
                  "%lu/%lu frames",
             (long long)(d->mc_stage_sum_us[0] / n), (long long)d->mc_stage_max_us[0],
             (long long)(d->mc_stage_sum_us[1] / n), (long long)d->mc_stage_max_us[1],
             (long long)(d->mc_stage_sum_us[2] / n), (long long)d->mc_stage_max_us[2],
             (unsigned long)d->mc_fast_frames, (unsigned long)d->mc_time_frames);
  }
  d->mc_time_sum_us = 0;
  d->mc_time_max_us = 0;
  d->mc_time_frames = 0;
  memset(d->mc_stage_sum_us, 0, sizeof(d->mc_stage_sum_us));
  memset(d->mc_stage_max_us, 0, sizeof(d->mc_stage_max_us));
  d->mc_stage_frames = 0;
  d->mc_fast_frames = 0;
}
#endif /* AAC_MULTICHANNEL */

static int mc_decode(aac_decoder_t *d, const uint8_t *data, size_t len,
                     float *output, size_t output_capacity_frames,
                     aac_decode_info_t *info) {
  UCHAR *in = (UCHAR *)data;
  UINT in_size = (UINT)len;
  UINT valid = (UINT)len;

#if AAC_MULTICHANNEL
  mc_apply_speaker(d);
  const int64_t t0 = esp_timer_get_time();
#endif
  mc_trace(MC_STAGE_FILL, d->mc_decodes);
  AAC_DECODER_ERROR err = aacDecoder_Fill(d->fdk, &in, &in_size, &valid);
  if (err == AAC_DEC_OK) {
    mc_trace(MC_STAGE_DECODE, d->mc_decodes);
    err = aacDecoder_DecodeFrame32(d->fdk, d->mc_pcm,
                                 (INT)(MC_MAX_FRAMES * MC_MAX_CHANNELS), 0);
  }
#if AAC_MULTICHANNEL
  const int64_t t1 = esp_timer_get_time();
#endif
  mc_trace(MC_STAGE_POST, d->mc_decodes);

  if (err != AAC_DEC_OK) {
    if ((++d->decode_errors & 63U) == 1U || d->decode_errors <= 8U)
      AP_LOGW(TAG, "FDK decode error 0x%x (in %u B, %u left; %lu total)",
               err, (unsigned)len, (unsigned)valid,
               (unsigned long)d->decode_errors);
    return -1;
  }

  const CStreamInfo *si = aacDecoder_GetStreamInfo(d->fdk);
  if (!si || si->numChannels <= 0 || si->numChannels > MC_MAX_CHANNELS ||
      si->frameSize <= 0 || (size_t)si->frameSize > MC_MAX_FRAMES) {
    return -1;
  }
#if !AAC_MULTICHANNEL
  if (si->numChannels > 2) return -1;
#endif
#if AAC_MULTICHANNEL
  INT fast = 0;
  INT64 synth_start = 0, synth_end = 0;
  if (si->numChannels > 2) {
    aacDecoder_GetSynthInfo(d->fdk, &fast, &synth_start, &synth_end);
    if (!d->mc_layout_known) {
      mc_build_speaker_map(d, si);
      mc_apply_speaker(d); /* narrows synthesis from the next frame on */
    }
    if (synth_start >= t0 && synth_end >= synth_start && t1 >= synth_end) {
      mc_account_stage(d, 0, synth_start - t0);
      mc_account_stage(d, 1, synth_end - synth_start);
      mc_account_stage(d, 2, t1 - synth_end);
      ++d->mc_stage_frames;
    }
    if (fast) ++d->mc_fast_frames;
    mc_account_time(d, t1 - t0, si->frameSize, si->sampleRate);
  }
#endif
  ++d->mc_decodes;
  mc_trace(MC_STAGE_IDLE, d->mc_decodes);

  const int ch = si->numChannels;
  size_t frames = (size_t)si->frameSize;
#if AAC_MULTICHANNEL
  if (ch > 2) mc_log_peaks(d, ch, frames, fast != 0);
#endif
  if (frames > output_capacity_frames) frames = output_capacity_frames;

#if AAC_MULTICHANNEL
  const int pick = d->mc_play_channel;
  if (fast) {
    /* FDK wrote just the played channel, mono. */
    mc_pick_channel(d->mc_pcm, 1, 0, frames, output);
  } else if (ch >= 3 && pick >= 0 && pick < ch) {
    mc_pick_channel(d->mc_pcm, ch, pick, frames, output);
  } else if (ch >= 3) {
    mc_downmix_to_mono(d->mc_pcm, ch, d->mc_lfe_index, frames, output);
  } else
#endif
  if (ch == 2) {
    for (size_t i = 0; i < frames * 2U; ++i)
      output[i] = audio_pcm32_to_float(d->mc_pcm[i]);
  } else {
    mc_pick_channel(d->mc_pcm, 1, 0, frames, output);
  }
  if (info) info->channels = 2;
  return (int)frames;
}

void aac_decoder_report_last_run(void) {
#if AAC_MULTICHANNEL
  static const char *const stages[] = {"idle", "scratch alloc",
                                       "aacDecoder_Open", "aacDecoder_SetParam",
                                       "aacDecoder_Fill", "aacDecoder_DecodeFrame32",
                                       "after decode"};
  const esp_reset_reason_t why = esp_reset_reason();
  if (s_mc_trace_magic == MC_TRACE_MAGIC && why != ESP_RST_POWERON) {
    const uint32_t st = s_mc_trace_stage;
    ESP_LOGW(TAG, "MC: last reset reason %d; FDK step at reset: %s, after %lu "
                  "decoded frames",
             (int)why,
             st < sizeof(stages) / sizeof(stages[0]) ? stages[st] : "?",
             (unsigned long)s_mc_trace_frames);
  } else {
    ESP_LOGW(TAG, "MC: last reset reason %d; no FDK trace", (int)why);
  }
  mc_trace(MC_STAGE_IDLE, 0);
#endif
}

const char *aac_speaker_name(int speaker) {
  static const char *const names[AAC_SPEAKER_COUNT] = {
      "Front Left (L)",  "Front Right (R)", "Centre (C)",
      "Subwoofer (LFE)", "Side Left (Ls)",  "Side Right (Rs)",
      "Rear Left (Lrs)", "Rear Right (Rrs)"};
  return (speaker >= 0 && speaker < AAC_SPEAKER_COUNT) ? names[speaker]
                                                       : "Mono mix";
}

void aac_decoder_set_speaker(int speaker) {
  if (speaker < AAC_SPEAKER_MIX || speaker >= AAC_SPEAKER_COUNT)
    speaker = AAC_SPEAKER_MIX;
#if AAC_MULTICHANNEL
  __atomic_store_n(&s_mc_speaker, speaker, __ATOMIC_RELAXED);
#else
  (void)speaker;
#endif
}

int aac_decoder_get_speaker(void) {
#if AAC_MULTICHANNEL
  return __atomic_load_n(&s_mc_speaker, __ATOMIC_RELAXED);
#else
  return AAC_SPEAKER_MIX;
#endif
}

bool aac_decoder_get_mc_status(int *output_channel, int *stream_channels,
                               char *label, size_t label_len) {
#if AAC_MULTICHANNEL
  taskENTER_CRITICAL(&s_mc_status_mux);
  const bool active = s_mc_status_active;
  if (output_channel) *output_channel = s_mc_status_channel;
  if (stream_channels) *stream_channels = s_mc_status_channels;
  if (label && label_len) strlcpy(label, s_mc_status_label, label_len);
  taskEXIT_CRITICAL(&s_mc_status_mux);
  return active;
#else
  (void)output_channel;
  (void)stream_channels;
  if (label && label_len) label[0] = '\0';
  return false;
#endif
}

aac_decoder_t *aac_decoder_create(const aac_decoder_config_t *config) {
  if (!config || config->sample_rate <= 0 || config->channels <= 0 ||
      config->channels > MC_MAX_CHANNELS) {
    return NULL;
  }
#if !AAC_MULTICHANNEL
  if (config->channels > 2) {
    ESP_LOGE(TAG, "%d-channel AAC needs CONFIG_AIRPLAY_MULTICHANNEL",
             config->channels);
    return NULL;
  }
#endif

  aac_decoder_t *d = calloc(1, sizeof(*d));
  if (!d) {
    return NULL;
  }
  d->config = *config;
  /* A full interleaved first frame is needed before surround selection.
   * Reserve the maximum even for stereo so an unexpected layout is safe. */
  const size_t bytes = MC_MAX_FRAMES * MC_MAX_CHANNELS * sizeof(*d->mc_pcm);
  mc_trace(MC_STAGE_ALLOC, 0);
  d->mc_pcm = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!d->mc_pcm) d->mc_pcm = malloc(bytes);
  if (!d->mc_pcm || !open_fdk_decoder(d)) {
    ESP_LOGE(TAG, "Cannot create FDK S32 AAC decoder");
    aac_decoder_destroy(d);
    return NULL;
  }
  return d;
}

void aac_decoder_destroy(aac_decoder_t *d) {
  if (!d) {
    return;
  }
  if (d->fdk) {
    aacDecoder_Close(d->fdk);
#if AAC_MULTICHANNEL
    mc_publish_status(false, -1, 0, NULL);
#endif
  }
  free(d->mc_pcm);
  free(d);
}

int aac_decoder_decode(aac_decoder_t *d, uint8_t *input,
                       size_t input_len, float *output,
                       size_t output_capacity_frames,
                       aac_decode_info_t *info) {
  const bool ready = d && d->fdk;
  if (!ready || !input || input_len == 0 || !output ||
      output_capacity_frames == 0) {
    return -1;
  }

  const uint8_t *decode_data = input;
  size_t decode_len = input_len;
  if (!has_adts(input, input_len)) {
    if (input_len > AAC_INPUT_MAX || input_len > 8191U - ADTS_HEADER_LEN) {
      return -1;
    }
    make_adts(input - ADTS_HEADER_LEN, input_len, d->config.sample_rate,
              d->config.channels);
    decode_data = input - ADTS_HEADER_LEN;
    decode_len = input_len + ADTS_HEADER_LEN;
  }

  return mc_decode(d, decode_data, decode_len, output,
                    output_capacity_frames, info);
}
