/* USB player sources and containers (phase 3a). See gw_player_src.h. */

#include "gw_player_src.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_http_client.h"
#include "esp_log.h"

static const char *TAG = "gw_player_src";

/* ---- HTTP ---- */

static int http_read(player_source_t *s, uint8_t *buf, size_t len) {
  esp_http_client_handle_t c = s->ctx;
  for (int tries = 0; tries < 20; ++tries) {
    const int n = esp_http_client_read(c, (char *)buf, (int)len);
    if (n != -ESP_ERR_HTTP_EAGAIN) {
      if (n == 0 && !esp_http_client_is_complete_data_received(c)) return -1;
      return n;
    }
  }
  return -1;
}

static void http_close(player_source_t *s) {
  esp_http_client_handle_t c = s->ctx;
  if (c) {
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
  }
  s->ctx = NULL;
}

esp_err_t player_source_http_open(player_source_t *s, const char *url) {
  memset(s, 0, sizeof(*s));
  const esp_http_client_config_t cfg = {
      .url = url,
      .timeout_ms = 5000,
      .buffer_size = 4096,
  };
  esp_http_client_handle_t c = esp_http_client_init(&cfg);
  if (!c) return ESP_ERR_NO_MEM;
  esp_err_t err = esp_http_client_open(c, 0);
  if (err != ESP_OK) {
    esp_http_client_cleanup(c);
    return err;
  }
  const int64_t len = esp_http_client_fetch_headers(c);
  const int status = esp_http_client_get_status_code(c);
  if (status != 200) {
    ESP_LOGW(TAG, "HTTP %d for %s", status, url);
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return ESP_ERR_NOT_FOUND;
  }
  s->ctx = c;
  s->read = http_read;
  s->close = http_close;
  s->size = len > 0 ? len : -1;
  return ESP_OK;
}

/* ---- WAV ---- */

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t le32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* WAVE_FORMAT_EXTENSIBLE speaker bits */
#define SPK_FL 0x1U
#define SPK_FR 0x2U
#define SPK_FC 0x4U
#define SPK_LFE 0x8U
#define SPK_BL 0x10U
#define SPK_BR 0x20U
#define SPK_SL 0x200U
#define SPK_SR 0x400U

/* Internal slots: 0 L, 1 R, 2 C, 3 LFE, 4 Ls, 5 Rs, 6 Lrs, 7 Rrs. The
 * file's channels come in the order of the mask's bits. */
static bool map_channels(wav_info_t *w) {
  const uint32_t m = w->mask;
  int8_t bit_slot[11];
  memset(bit_slot, -1, sizeof(bit_slot));
  if (w->channels == 1) {
    if (m != SPK_FC && m != SPK_FL) return false;
    w->slot[0] = 0;
    w->present = 1U;
    w->layout = 1;
    return true;
  }
  if (w->channels == 2 && m == (SPK_FL | SPK_FR)) {
    w->layout = 2;
  } else if (w->channels == 6 && (m == 0x3FU || m == 0x60FU)) {
    w->layout = 6; /* 5.1 with back or side surrounds: both are Ls/Rs */
  } else if (w->channels == 8 && m == 0x63FU) {
    w->layout = 8; /* 7.1: sides are Ls/Rs, backs Lrs/Rrs */
  } else {
    return false;
  }
  bit_slot[0] = 0;  /* FL */
  bit_slot[1] = 1;  /* FR */
  bit_slot[2] = 2;  /* FC */
  bit_slot[3] = 3;  /* LFE */
  bit_slot[9] = 4;  /* SL */
  bit_slot[10] = 5; /* SR */
  if (w->layout == 8) {
    bit_slot[4] = 6; /* BL */
    bit_slot[5] = 7; /* BR */
  } else {
    bit_slot[4] = 4; /* 5.1 back pair as Ls/Rs */
    bit_slot[5] = 5;
  }
  int ch = 0;
  w->present = 0;
  for (int b = 0; b < 11 && ch < w->channels; ++b) {
    if (!(m & (1U << b))) continue;
    w->slot[ch] = bit_slot[b];
    if (bit_slot[b] >= 0) w->present |= (uint8_t)(1U << bit_slot[b]);
    ch++;
  }
  return ch == w->channels;
}

size_t id3v2_size(const uint8_t *buf, size_t len) {
  if (len < 10 || memcmp(buf, "ID3", 3) != 0) return 0;
  const size_t body = ((size_t)(buf[6] & 0x7F) << 21) | ((size_t)(buf[7] & 0x7F) << 14) |
                      ((size_t)(buf[8] & 0x7F) << 7) | (buf[9] & 0x7F);
  return 10U + body + ((buf[5] & 0x10) ? 10U : 0U); /* footer */
}

esp_err_t flac_parse(const uint8_t *buf, size_t len, wav_info_t *w, uint64_t *total) {
  memset(w, 0, sizeof(*w));
  memset(w->slot, -1, sizeof(w->slot));
  *total = 0;
  if (len < 4 + 4 + 34) return ESP_ERR_INVALID_SIZE;
  if (memcmp(buf, "fLaC", 4) != 0) {
    strlcpy(w->why, "Not a FLAC file", sizeof(w->why));
    return ESP_ERR_NOT_SUPPORTED;
  }
  const uint8_t *b = buf + 4;
  if ((b[0] & 0x7F) != 0) return ESP_FAIL; /* STREAMINFO must come first */
  const uint8_t *si = b + 4;
  w->rate = (int)(((uint32_t)si[10] << 12) | ((uint32_t)si[11] << 4) | (si[12] >> 4));
  w->channels = ((si[12] >> 1) & 0x07) + 1;
  w->bits = (((si[12] & 0x01) << 4) | (si[13] >> 4)) + 1;
  *total = ((uint64_t)(si[13] & 0x0F) << 32) | ((uint64_t)si[14] << 24) |
           ((uint64_t)si[15] << 16) | ((uint64_t)si[16] << 8) | si[17];
  w->data_offset = 0; /* the decoder takes the file from its start */
  if (w->rate != 44100 && w->rate != 48000) {
    snprintf(w->why, sizeof(w->why), "%d Hz (phase 3b plays 44.1 and 48 kHz)", w->rate);
    return ESP_ERR_NOT_SUPPORTED;
  }
  if (w->bits != 16 && w->bits != 24) {
    snprintf(w->why, sizeof(w->why), "%d-bit FLAC (16 and 24 only)", w->bits);
    return ESP_ERR_NOT_SUPPORTED;
  }
  static const int8_t k6[6] = {0, 1, 2, 3, 4, 5};       /* BL/BR as Ls/Rs */
  static const int8_t k8[8] = {0, 1, 2, 3, 6, 7, 4, 5}; /* BL/BR = Lrs/Rrs, SL/SR = Ls/Rs */
  const int8_t *map = NULL;
  switch (w->channels) {
    case 1: w->slot[0] = 0; w->layout = 1; break;
    case 2: w->slot[0] = 0; w->slot[1] = 1; w->layout = 2; break;
    case 6: map = k6; w->layout = 6; break;
    case 8: map = k8; w->layout = 8; break;
    default:
      snprintf(w->why, sizeof(w->why), "%d channels (mono, stereo, 5.1, 7.1)", w->channels);
      return ESP_ERR_NOT_SUPPORTED;
  }
  if (map) memcpy(w->slot, map, (size_t)w->channels);
  for (int c = 0; c < w->channels; ++c)
    if (w->slot[c] >= 0) w->present |= (uint8_t)(1U << w->slot[c]);
  return ESP_OK;
}

esp_err_t wav_parse(const uint8_t *buf, size_t len, wav_info_t *w) {
  memset(w, 0, sizeof(*w));
  memset(w->slot, -1, sizeof(w->slot));
  if (len < 12) return ESP_ERR_INVALID_SIZE;
  if (memcmp(buf, "RIFF", 4) != 0 || memcmp(buf + 8, "WAVE", 4) != 0) {
    strlcpy(w->why, "Not a WAV file", sizeof(w->why));
    return ESP_ERR_NOT_SUPPORTED;
  }
  size_t at = 12;
  bool have_fmt = false;
  while (at + 8 <= len) {
    const uint8_t *ck = buf + at;
    const uint32_t size = le32(ck + 4);
    if (memcmp(ck, "fmt ", 4) == 0) {
      if (at + 8 + size > len) return ESP_ERR_INVALID_SIZE;
      if (size < 16) return ESP_FAIL;
      const uint8_t *f = ck + 8;
      const uint16_t tag = le16(f);
      w->channels = le16(f + 2);
      w->rate = (int)le32(f + 4);
      w->container_bits = le16(f + 14);
      w->bits = w->container_bits;
      if (tag == 0xFFFEU && size >= 40) {
        w->bits = le16(f + 18);
        w->mask = le32(f + 20);
        /* the sub-format GUID's first two bytes: 1 = PCM */
        if (le16(f + 24) != 1U) {
          strlcpy(w->why, "Not integer PCM (float or compressed WAV)", sizeof(w->why));
          return ESP_ERR_NOT_SUPPORTED;
        }
      } else if (tag != 1U) {
        strlcpy(w->why, "Not integer PCM (float or compressed WAV)", sizeof(w->why));
        return ESP_ERR_NOT_SUPPORTED;
      }
      if (!w->mask) {
        w->mask = w->channels == 1 ? SPK_FC : w->channels == 2 ? (SPK_FL | SPK_FR)
                : w->channels == 6 ? 0x3FU : w->channels == 8 ? 0x63FU : 0U;
      }
      have_fmt = true;
    } else if (memcmp(ck, "data", 4) == 0) {
      if (!have_fmt) return ESP_FAIL;
      w->data_offset = at + 8;
      w->data_size = size;
      break;
    }
    at += 8 + size + (size & 1U);
  }
  if (!w->data_offset) return ESP_ERR_INVALID_SIZE;

  if (w->rate != 44100 && w->rate != 48000) {
    snprintf(w->why, sizeof(w->why), "%d Hz (phase 3a plays 44.1 and 48 kHz)", w->rate);
    return ESP_ERR_NOT_SUPPORTED;
  }
  const bool depth_ok = (w->container_bits == 16 && w->bits == 16) ||
                        (w->container_bits == 24 && w->bits == 24) ||
                        (w->container_bits == 32 && w->bits == 24);
  if (!depth_ok) {
    snprintf(w->why, sizeof(w->why), "%d-bit samples in %d bits (16 and 24 only)", w->bits,
             w->container_bits);
    return ESP_ERR_NOT_SUPPORTED;
  }
  if (!map_channels(w)) {
    snprintf(w->why, sizeof(w->why), "%d channels, mask 0x%" PRIx32 " (mono, stereo, 5.1, 7.1)",
             w->channels, w->mask);
    return ESP_ERR_NOT_SUPPORTED;
  }
  return ESP_OK;
}
