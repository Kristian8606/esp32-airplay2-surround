/* SPDX-License-Identifier: Apache-2.0 */
#include "rtsp_tables.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "airplay_identity.h"

int32_t rtsp_volume_to_q15(double volume) {
  double f;
  if (isnan(volume) || volume < RTSP_VOLUME_MIN) return 0;
  if (volume > RTSP_VOLUME_MAX) volume = RTSP_VOLUME_MAX;
  f = (volume + 30.0) / 30.0;
  return (int32_t)(f * f * 32768.0);
}

double rtsp_volume_to_fraction(double volume) {
  double f;
  if (isnan(volume) || volume < RTSP_VOLUME_MIN) return 0.0;
  f = (volume + 30.0) / 30.0;
  return f > 1.0 ? 1.0 : f;
}

double rtsp_fraction_to_volume(double fraction) {
  if (fraction < 0.0) fraction = 0.0;
  if (fraction > 1.0) fraction = 1.0;
  return fraction * 30.0 - 30.0;
}

double rtsp_volume_step(double volume, bool up) {
  double f = rtsp_volume_to_fraction(volume) + (up ? 1.0 : -1.0) / 16.0;
  if (f < 0.0) f = 0.0;
  if (f > 1.0) f = 1.0;
  return f;
}

bool rtsp_parse_volume(const char *text, double *volume) {
  char *end;
  double v;
  while (*text == ' ' || *text == '\t') text++;
  if (*text == '\0') return false;
  errno = 0;
  v = strtod(text, &end);
  if (end == text || errno == ERANGE || isnan(v) || isinf(v)) return false;
  while (*end == ' ' || *end == '\t') end++;
  if (*end != '\0') return false;
  *volume = v;
  return true;
}

double rtsp_volume_clamp(double volume) {
  if (volume > RTSP_VOLUME_MAX) return RTSP_VOLUME_MAX;
  if (volume < RTSP_VOLUME_MUTE) return RTSP_VOLUME_MUTE;
  return volume;
}

void rtsp_format_volume(double volume, char *out, size_t capacity) {
  snprintf(out, capacity, "%.2f", volume);
}

typedef struct {
  uint64_t bit;
  bool realtime_too; /* also for type 96 */
  const char *codec;
  int sample_rate;
  int bits;
  int frames;
} format_row_t;

/* Channels are 2 for every row, also 5.1 and 7.1: the decoder finds the
 * channels in the stream. */
static const format_row_t FORMATS[] = {
    {AIRPLAY_FMT_ALAC_44100_S16_2, true, "ALAC", 44100, 16, 352},
    {AIRPLAY_FMT_ALAC_48000_S24_2, false, "ALAC", 48000, 24, 352},
    {AIRPLAY_FMT_AAC_LC_44100_2, false, "AAC", 44100, 24, 1024},
    {AIRPLAY_FMT_AAC_LC_48000_2, false, "AAC", 48000, 24, 1024},
    {AIRPLAY_FMT_AAC_LC_48000_5_1, false, "AAC", 48000, 24, 1024},
    {AIRPLAY_FMT_AAC_LC_48000_7_1, false, "AAC", 48000, 24, 1024},
};

bool rtsp_format_lookup(int64_t stream_type, uint64_t audio_format,
                        audio_format_t *format) {
  if (stream_type != AUDIO_STREAM_REALTIME &&
      stream_type != AUDIO_STREAM_BUFFERED) {
    return false;
  }
  for (size_t i = 0; i < sizeof(FORMATS) / sizeof(FORMATS[0]); i++) {
    const format_row_t *row = &FORMATS[i];
    if (row->bit != audio_format) continue;
    if (stream_type == AUDIO_STREAM_REALTIME && !row->realtime_too) {
      return false;
    }
    memset(format, 0, sizeof(*format));
    snprintf(format->codec, sizeof(format->codec), "%s", row->codec);
    format->sample_rate = row->sample_rate;
    format->channels = 2;
    format->bits_per_sample = row->bits;
    format->frame_size = row->frames;
    return true;
  }
  return false;
}

uint64_t rtsp_network_time_ns(uint64_t secs, uint64_t frac) {
  /* frac * 10^9 / 2^64 without 128-bit arithmetic: split frac into 32-bit
   * halves; each product fits in 64 bits. */
  uint64_t hi = frac >> 32;
  uint64_t lo = frac & 0xffffffffULL;
  uint64_t frac_ns = (hi * 1000000000ULL + ((lo * 1000000000ULL) >> 32)) >> 32;
  return secs * 1000000000ULL + frac_ns;
}

bool rtsp_next_line(const char **cursor, const char *end, const char **line,
                    size_t *line_len) {
  const char *p = *cursor;
  const char *nl;
  size_t n;
  if (p >= end) return false;
  nl = memchr(p, '\n', (size_t)(end - p));
  n = nl != NULL ? (size_t)(nl - p) : (size_t)(end - p);
  *cursor = nl != NULL ? nl + 1 : end;
  if (n > 0 && p[n - 1] == '\r') n--;
  *line = p;
  *line_len = n;
  return true;
}

static bool copy_trimmed(const char *s, size_t n, char *out, size_t cap) {
  while (n > 0 && (*s == ' ' || *s == '\t')) {
    s++;
    n--;
  }
  while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t')) n--;
  if (n + 1 > cap) return false;
  memcpy(out, s, n);
  out[n] = '\0';
  return true;
}

bool rtsp_split_param(const char *line, size_t line_len, char *name,
                      size_t name_capacity, char *value,
                      size_t value_capacity) {
  const char *colon = memchr(line, ':', line_len);
  if (colon == NULL) return false;
  return copy_trimmed(line, (size_t)(colon - line), name, name_capacity) &&
         copy_trimmed(colon + 1, line_len - (size_t)(colon - line) - 1, value,
                      value_capacity);
}

bool rtsp_parse_rtp_info(const char *header, uint32_t *rtptime) {
  const char *p = header;
  static const char KEY[] = "rtptime=";
  while (*p) {
    while (*p == ' ' || *p == ';' || *p == ',') p++;
    if (strncmp(p, KEY, sizeof(KEY) - 1) == 0) {
      uint64_t v = 0;
      const char *d = p + sizeof(KEY) - 1;
      if (*d < '0' || *d > '9') return false;
      for (; *d >= '0' && *d <= '9'; d++) {
        v = v * 10 + (uint64_t)(*d - '0');
        if (v > 0xffffffffULL) return false;
      }
      if (*d != '\0' && *d != ';' && *d != ',' && *d != ' ') return false;
      *rtptime = (uint32_t)v;
      return true;
    }
    while (*p && *p != ';' && *p != ',') p++;
  }
  return false;
}

static bool media_type_prefix(const char *header, const char *type,
                              bool prefix_only) {
  size_t n = strlen(type);
  if (header == NULL) return false;
  while (*header == ' ') header++;
  for (size_t i = 0; i < n; i++) {
    char a = header[i], b = type[i];
    if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
    if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
    if (a != b) return false;
  }
  if (prefix_only) return header[n] != '\0' && header[n] != ';';
  return header[n] == '\0' || header[n] == ';' || header[n] == ' ';
}

bool rtsp_content_type_is(const char *header, const char *type) {
  return media_type_prefix(header, type, false);
}

bool rtsp_content_type_is_image(const char *header) {
  return media_type_prefix(header, "image/", true);
}
