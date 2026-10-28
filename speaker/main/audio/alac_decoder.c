#include "alac_decoder.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "decoder/impl/esp_alac_dec.h"
#include "esp_audio_dec.h"
#include "esp_log.h"
#include "airplay_log.h"

#define ALAC_COOKIE_LEN 24U

struct alac_decoder {
  alac_decoder_config_t config;
  void *handle;
  uint32_t decode_errors;
  bool short_consume_logged;
  uint8_t *pcm;
  uint32_t pcm_size;
  esp_audio_dec_info_t info;
  uint8_t cookie[ALAC_COOKIE_LEN];
};

static const char *TAG = "alac_dec_v1";

static void put_be16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)(v >> 8);
  p[1] = (uint8_t)v;
}

static void put_be32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24);
  p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8);
  p[3] = (uint8_t)v;
}

/* Build the standard 24-byte ALACSpecificConfig ("magic cookie").
 * AirPlay realtime tells us the fields that matter here: frame size, bit
 * depth, channel count and sample rate. The ALAC reference defaults are
 * pb=40, mb=10, kb=14, maxRun=255. maxFrameBytes/avgBitRate may be zero when
 * unknown. Multi-byte fields in the cookie are big-endian. */
static void make_cookie(alac_decoder_t *d) {
  memset(d->cookie, 0, sizeof(d->cookie));
  put_be32(d->cookie + 0, (uint32_t)d->config.frame_size);
  d->cookie[4] = 0; /* compatibleVersion */
  d->cookie[5] = (uint8_t)d->config.bits_per_sample;
  d->cookie[6] = 40; /* pb */
  d->cookie[7] = 10; /* mb */
  d->cookie[8] = 14; /* kb */
  d->cookie[9] = (uint8_t)d->config.channels;
  put_be16(d->cookie + 10, 255);
  put_be32(d->cookie + 12, 0); /* maxFrameBytes unknown */
  put_be32(d->cookie + 16, 0); /* avgBitRate unknown */
  put_be32(d->cookie + 20, (uint32_t)d->config.sample_rate);
}

static bool open_decoder(alac_decoder_t *d) {
  if (d->handle) {
    esp_alac_dec_close(d->handle);
    d->handle = NULL;
  }

  make_cookie(d);
  esp_alac_dec_cfg_t cfg = ESP_ALAC_DEC_CONFIG_DEFAULT();
  cfg.codec_spec_info = d->cookie;
  cfg.spec_info_len = sizeof(d->cookie);
  esp_audio_err_t err = esp_alac_dec_open(&cfg, sizeof(cfg), &d->handle);
  if (err != ESP_AUDIO_ERR_OK) {
    ESP_LOGE(TAG, "esp_alac_dec_open failed: %d", err);
    d->handle = NULL;
    return false;
  }
  return true;
}

alac_decoder_t *alac_decoder_create(const alac_decoder_config_t *config) {
  if (!config || config->sample_rate <= 0 || config->channels <= 0 ||
      config->channels > 8 ||
      (config->bits_per_sample != 16 && config->bits_per_sample != 24) ||
      config->frame_size <= 0 ||
      (size_t)config->frame_size >
          UINT32_MAX / ((size_t)config->channels * config->bits_per_sample)) {
    return NULL;
  }

  alac_decoder_t *d = calloc(1, sizeof(*d));
  if (!d) {
    return NULL;
  }
  d->config = *config;
  d->pcm_size = (uint32_t)((size_t)config->frame_size * config->channels *
                           (config->bits_per_sample / 8));
  d->pcm = malloc(d->pcm_size);
  if (!d->pcm) {
    alac_decoder_destroy(d);
    return NULL;
  }
  if (!open_decoder(d)) {
    alac_decoder_destroy(d);
    return NULL;
  }
  return d;
}

void alac_decoder_destroy(alac_decoder_t *d) {
  if (!d) {
    return;
  }
  if (d->handle) {
    esp_alac_dec_close(d->handle);
  }
  free(d->pcm);
  free(d);
}

static int decode_pcm(alac_decoder_t *d, const uint8_t *input,
                      size_t input_len, size_t output_capacity_frames,
                      alac_decode_info_t *info) {
  if (!d || !d->handle || !input || input_len == 0 || input_len > UINT32_MAX ||
      output_capacity_frames == 0) {
    return -1;
  }

  const size_t frame_bytes =
      (size_t)d->config.channels * (d->config.bits_per_sample / 8);
  esp_audio_dec_in_raw_t raw = {
      .buffer = (uint8_t *)input,
      .len = (uint32_t)input_len,
      .consumed = 0,
      .frame_recover = ESP_AUDIO_DEC_RECOVERY_NONE,
  };
  esp_audio_dec_out_frame_t frame = {
      .buffer = d->pcm,
      .len = d->pcm_size,
      .decoded_size = 0,
  };
  esp_audio_dec_info_t dec_info = d->info;

  esp_audio_err_t err = esp_alac_dec_decode(d->handle, &raw, &frame, &dec_info);
  if (err == ESP_AUDIO_ERR_OK) {
    /* A field the decoder leaves at 0 is unknown, not a mismatch: the
     * realtime path decoded for years without these being reported. */
    if ((dec_info.channel != 0 && dec_info.channel != d->config.channels) ||
        (dec_info.sample_rate != 0 &&
         dec_info.sample_rate != (uint32_t)d->config.sample_rate) ||
        (dec_info.bits_per_sample != 0 &&
         dec_info.bits_per_sample != d->config.bits_per_sample)) {
      err = ESP_AUDIO_ERR_INVALID_PARAMETER;
    } else {
      d->info = dec_info;
      /* The decoder may stop before trailing alignment bytes of the
       * packet; only reading past the end is an error. */
      if (raw.consumed > input_len || frame.decoded_size > d->pcm_size ||
          frame.decoded_size % frame_bytes != 0 ||
          frame.decoded_size / frame_bytes > output_capacity_frames) {
        err = ESP_AUDIO_ERR_INVALID_PARAMETER;
      } else if (raw.consumed < input_len && !d->short_consume_logged) {
        d->short_consume_logged = true;
        ESP_LOGW(TAG, "ALAC packet: decoder used %lu of %u bytes; rest ignored",
                 (unsigned long)raw.consumed, (unsigned)input_len);
      }
    }
  }
  if (err != ESP_AUDIO_ERR_OK) {
    if ((++d->decode_errors & 63U) == 1U)
      AP_LOGW(TAG, "decode error=%d; resetting ALAC decoder (%lu total)",
               err, (unsigned long)d->decode_errors);
    esp_alac_dec_reset(d->handle);
    return -1;
  }

  if (info) {
    info->channels = dec_info.channel ? dec_info.channel : d->config.channels;
    info->sample_rate = dec_info.sample_rate ? (int)dec_info.sample_rate
                                             : d->config.sample_rate;
    info->bits_per_sample = dec_info.bits_per_sample ? dec_info.bits_per_sample
                                                     : d->config.bits_per_sample;
  }
  return (int)(frame.decoded_size / frame_bytes);
}

int alac_decoder_decode(alac_decoder_t *d, const uint8_t *input,
                        size_t input_len, int16_t *output,
                        size_t output_capacity_frames,
                        alac_decode_info_t *info) {
  if (!d || d->config.bits_per_sample != 16 || !output) {
    return -1;
  }
  int frames = decode_pcm(d, input, input_len, output_capacity_frames, info);
  if (frames > 0) {
    memcpy(output, d->pcm,
           (size_t)frames * d->config.channels * sizeof(int16_t));
  }
  return frames;
}

int alac_decoder_decode_float(alac_decoder_t *decoder, const uint8_t *input,
                              size_t input_len, float *output,
                              size_t output_capacity_frames,
                              alac_decode_info_t *info) {
  if (!decoder || decoder->config.channels != 2 || !output) {
    return -1;
  }
  int frames = decode_pcm(decoder, input, input_len, output_capacity_frames, info);
  if (frames <= 0) {
    return frames;
  }

  const size_t sample_bytes = (size_t)decoder->config.bits_per_sample / 8;
  const float scale = sample_bytes == 3 ? 0x1p-23f : 0x1p-15f;
  const int32_t sign = sample_bytes == 3 ? 0x800000 : 0x8000;
  const uint8_t *sample_data = decoder->pcm;
  for (size_t sample_index = 0; sample_index < (size_t)frames * 2;
       ++sample_index, sample_data += sample_bytes) {
    int32_t sample = (int32_t)sample_data[0] | ((int32_t)sample_data[1] << 8);
    if (sample_bytes == 3) {
      sample |= (int32_t)sample_data[2] << 16;
    }
    if (sample & sign) {
      sample -= sign * 2;
    }
    output[sample_index] = (float)sample * scale;
  }
  return frames;
}
