/* M4A index for the Gateway's player (phase 3d.2). See gw_player_m4a.h. */

#include "gw_player_m4a.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "gw_player_m4a";

#define M4A_MOOV_MAX (4U * 1024U * 1024U)

static uint32_t be32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static uint64_t be64(const uint8_t *p) { return ((uint64_t)be32(p) << 32) | be32(p + 4); }

/* The next child box in [*pos, len) of b: its type, body and body length. */
static bool next_box(const uint8_t *b, size_t len, size_t *pos, const uint8_t **type,
                     const uint8_t **body, size_t *blen) {
  if (*pos + 8 > len) return false;
  const uint8_t *h = b + *pos;
  uint64_t size = be32(h);
  size_t head = 8;
  if (size == 1) {
    if (*pos + 16 > len) return false;
    size = be64(h + 8);
    head = 16;
  } else if (size == 0) {
    size = len - *pos;
  }
  if (size < head || size > len - *pos) return false;
  *type = h + 4;
  *body = h + head;
  *blen = (size_t)size - head;
  *pos += (size_t)size;
  return true;
}

static const uint8_t *child(const uint8_t *b, size_t len, const char *want, size_t *blen) {
  size_t pos = 0;
  const uint8_t *type, *body;
  size_t n;
  while (next_box(b, len, &pos, &type, &body, &n)) {
    if (memcmp(type, want, 4) == 0) {
      *blen = n;
      return body;
    }
  }
  return NULL;
}

/* A child box found by its type anywhere in b (sample entries hold their
 * boxes after fields of a length that depends on the version). */
static const uint8_t *find_box(const uint8_t *b, size_t len, const char *want, size_t *blen) {
  for (size_t i = 4; i + 4 <= len; ++i) {
    if (memcmp(b + i, want, 4) != 0) continue;
    const uint32_t size = be32(b + i - 4);
    if (size < 8 || size - 8 > len - i - 4) continue;
    *blen = size - 8;
    return b + i + 4;
  }
  return NULL;
}

/* MPEG-4 descriptor: tag and length (1..4 bytes of 7 bits). */
static bool descr(const uint8_t **p, const uint8_t *end, uint8_t *tag, uint32_t *len) {
  if (*p >= end) return false;
  *tag = *(*p)++;
  *len = 0;
  for (int k = 0; k < 4; ++k) {
    if (*p >= end) return false;
    const uint8_t b = *(*p)++;
    *len = (*len << 7) | (b & 0x7F);
    if (!(b & 0x80)) return *len <= (uint32_t)(end - *p);
  }
  return false;
}

typedef struct {
  const uint8_t *b;
  size_t len, bit;
} bits_t;

static uint32_t getbits(bits_t *r, int n) {
  uint32_t v = 0;
  for (int k = 0; k < n; ++k, ++r->bit) {
    const size_t byte = r->bit >> 3;
    const uint8_t x = byte < r->len ? r->b[byte] : 0;
    v = (v << 1) | ((x >> (7 - (r->bit & 7))) & 1U);
  }
  return v;
}

static bool parse_asc(m4a_track_t *t, char *why, size_t why_len) {
  bits_t r = {t->config, t->config_len, 0};
  uint32_t aot = getbits(&r, 5);
  if (aot == 31) aot = 32 + getbits(&r, 6);
  uint32_t ix = getbits(&r, 4);
  if (ix == 15) {
    snprintf(why, why_len, "AAC at a rate ADTS cannot name");
    return false;
  }
  const uint32_t ch = getbits(&r, 4);
  if (aot == 5 || aot == 29) { /* HE-AAC, explicit: the core comes after */
    const uint32_t ext = getbits(&r, 4);
    if (ext == 15) (void)getbits(&r, 24);
    aot = getbits(&r, 5);
  }
  if (aot < 1 || aot > 4) {
    snprintf(why, why_len, "AAC object type %u (LC and HE-AAC only)", (unsigned)aot);
    return false;
  }
  if (ch == 0 || ch > 7) {
    snprintf(why, why_len, "AAC with channel configuration %u", (unsigned)ch);
    return false;
  }
  t->adts_profile = (uint8_t)(aot - 1);
  t->adts_rate_ix = (uint8_t)ix;
  t->adts_channels = (uint8_t)ch;
  return true;
}

static bool parse_stsd(m4a_track_t *t, const uint8_t *b, size_t len, char *why, size_t why_len) {
  if (len < 8 + 8 + 28) return false;
  const uint8_t *e = b + 8; /* the first entry */
  const uint32_t esize = be32(e);
  if (esize < 8 + 28 || esize > len - 8) return false;
  const uint8_t *fmt = e + 4;
  const uint8_t *body = e + 8;
  const size_t blen = esize - 8;
  t->channels = (int)((body[16] << 8) | body[17]);
  t->bits = (int)((body[18] << 8) | body[19]);
  t->rate = (int)(be32(body + 24) >> 16);
  if (memcmp(fmt, "alac", 4) == 0) {
    size_t n;
    const uint8_t *a = find_box(body + 28, blen - 28, "alac", &n);
    if (!a || (n != 28 && n != 24)) {
      snprintf(why, why_len, "ALAC without its configuration");
      return false;
    }
    memcpy(t->config, a + (n == 28 ? 4 : 0), 24);
    t->config_len = 24;
    t->codec = M4A_ALAC;
    t->bits = t->config[5];
    t->channels = t->config[9];
    t->rate = (int)be32(t->config + 20);
    return true;
  }
  if (memcmp(fmt, ".mp3", 4) == 0) {
    t->codec = M4A_MP3;
    t->bits = 16;
    return true;
  }
  if (memcmp(fmt, "mp4a", 4) != 0) {
    snprintf(why, why_len, "M4A with %.4s audio (ALAC, AAC and MP3 only)", (const char *)fmt);
    return false;
  }
  size_t n;
  const uint8_t *es = find_box(body + 28, blen - 28, "esds", &n);
  if (!es || n < 4) {
    snprintf(why, why_len, "AAC without its configuration (esds)");
    return false;
  }
  const uint8_t *p = es + 4, *end = es + n;
  uint8_t tag;
  uint32_t dl;
  if (!descr(&p, end, &tag, &dl) || tag != 0x03 || end - p < 3) goto bad;
  {
    const uint8_t flags = p[2];
    p += 3;
    if (flags & 0x80) p += 2;
    if (flags & 0x40) p += 1 + (p < end ? *p : 0);
    if (flags & 0x20) p += 2;
  }
  if (!descr(&p, end, &tag, &dl) || tag != 0x04 || dl < 13) goto bad;
  {
    const uint8_t oti = p[0];
    p += 13;
    if (oti == 0x6B || oti == 0x69) { /* MP3 in mp4a */
      t->codec = M4A_MP3;
      t->bits = 16;
      return true;
    }
    if (oti != 0x40 && oti != 0x67) {
      snprintf(why, why_len, "M4A with object type 0x%02x (AAC or MP3 only)", oti);
      return false;
    }
  }
  if (!descr(&p, end, &tag, &dl) || tag != 0x05 || !dl || dl > sizeof(t->config)) goto bad;
  memcpy(t->config, p, dl);
  t->config_len = dl;
  t->codec = M4A_AAC;
  t->bits = 16;
  return parse_asc(t, why, why_len);
bad:
  snprintf(why, why_len, "AAC with a configuration it cannot read");
  return false;
}

static void *ps_alloc(size_t n) { return heap_caps_malloc(n ? n : 1, MALLOC_CAP_SPIRAM); }

static bool parse_stbl(m4a_track_t *t, const uint8_t *stbl, size_t len, char *why,
                       size_t why_len) {
  size_t n;
  const uint8_t *b = child(stbl, len, "stsd", &n);
  if (!b || !parse_stsd(t, b, n, why, why_len)) {
    if (!why[0]) snprintf(why, why_len, "M4A without a sample description");
    return false;
  }
  if ((b = child(stbl, len, "stts", &n)) == NULL || n < 8) goto bad;
  t->n_stts = be32(b + 4);
  if ((uint64_t)t->n_stts * 8U > n - 8) goto bad;
  t->stts = ps_alloc(t->n_stts * sizeof(*t->stts));
  if (!t->stts) goto mem;
  for (uint32_t i = 0; i < t->n_stts; ++i) {
    t->stts[i].count = be32(b + 8 + i * 8);
    t->stts[i].delta = be32(b + 12 + i * 8);
  }
  if ((b = child(stbl, len, "stsc", &n)) == NULL || n < 8) goto bad;
  t->n_stsc = be32(b + 4);
  if (!t->n_stsc || (uint64_t)t->n_stsc * 12U > n - 8) goto bad;
  t->stsc = ps_alloc(t->n_stsc * sizeof(*t->stsc));
  if (!t->stsc) goto mem;
  for (uint32_t i = 0; i < t->n_stsc; ++i) {
    t->stsc[i].first_chunk = be32(b + 8 + i * 12);
    t->stsc[i].per_chunk = be32(b + 12 + i * 12);
    if (!t->stsc[i].first_chunk || !t->stsc[i].per_chunk) goto bad;
  }
  if ((b = child(stbl, len, "stsz", &n)) == NULL || n < 12) {
    snprintf(why, why_len, "M4A without stsz (stz2 is not read)");
    return false;
  }
  t->const_size = be32(b + 4);
  t->samples = be32(b + 8);
  if (!t->const_size) {
    if ((uint64_t)t->samples * 4U > n - 12) goto bad;
    t->sizes = ps_alloc(t->samples * sizeof(uint32_t));
    if (!t->sizes) goto mem;
    for (uint32_t i = 0; i < t->samples; ++i) {
      t->sizes[i] = be32(b + 12 + i * 4);
      if (t->sizes[i] > t->max_size) t->max_size = t->sizes[i];
    }
  } else {
    t->max_size = t->const_size;
  }
  const bool co64 = (b = child(stbl, len, "co64", &n)) != NULL;
  if (!co64 && (b = child(stbl, len, "stco", &n)) == NULL) goto bad;
  if (n < 8) goto bad;
  t->chunks = be32(b + 4);
  if ((uint64_t)t->chunks * (co64 ? 8U : 4U) > n - 8) goto bad;
  t->chunk_off = ps_alloc(t->chunks * sizeof(uint64_t));
  if (!t->chunk_off) goto mem;
  for (uint32_t i = 0; i < t->chunks; ++i)
    t->chunk_off[i] = co64 ? be64(b + 8 + i * 8) : be32(b + 8 + i * 4);
  if (!t->samples || !t->chunks) {
    snprintf(why, why_len, "M4A with no audio samples");
    return false;
  }
  return true;
bad:
  snprintf(why, why_len, "M4A with a sample table it cannot read");
  return false;
mem:
  snprintf(why, why_len, "Out of memory for the M4A index");
  return false;
}

static bool parse_moov(m4a_track_t *t, const uint8_t *moov, size_t len, char *why,
                       size_t why_len) {
  size_t pos = 0;
  const uint8_t *type, *trak;
  size_t tlen;
  while (next_box(moov, len, &pos, &type, &trak, &tlen)) {
    if (memcmp(type, "trak", 4) != 0) continue;
    size_t n, hn, mn;
    const uint8_t *mdia = child(trak, tlen, "mdia", &n);
    if (!mdia) continue;
    const uint8_t *hdlr = child(mdia, n, "hdlr", &hn);
    if (!hdlr || hn < 12 || memcmp(hdlr + 8, "soun", 4) != 0) continue;
    const uint8_t *mdhd = child(mdia, n, "mdhd", &mn);
    if (mdhd && mn >= 24) {
      if (mdhd[0] == 1 && mn >= 36) {
        t->timescale = be32(mdhd + 20);
        t->duration_ts = be64(mdhd + 24);
      } else {
        t->timescale = be32(mdhd + 12);
        t->duration_ts = be32(mdhd + 16);
      }
    }
    size_t in, sn;
    const uint8_t *minf = child(mdia, n, "minf", &in);
    const uint8_t *stbl = minf ? child(minf, in, "stbl", &sn) : NULL;
    if (!stbl) continue;
    if (!t->timescale) t->timescale = 1;
    return parse_stbl(t, stbl, sn, why, why_len);
  }
  snprintf(why, why_len, "M4A without an audio track");
  return false;
}

esp_err_t m4a_open(player_source_t *src, int64_t base, m4a_track_t *t, char *why, size_t why_len) {
  memset(t, 0, sizeof(*t));
  why[0] = '\0';
  if (!src->read_at) {
    snprintf(why, why_len, "M4A needs a source that can seek");
    return ESP_ERR_NOT_SUPPORTED;
  }
  /* the top-level boxes: where moov is */
  int64_t at = base, moov_at = -1;
  uint64_t moov_size = 0;
  bool mdat_seen = false;
  for (int k = 0; k < 64 && (src->size < 0 || at + 8 <= src->size); ++k) {
    uint8_t h[16];
    if (src->read_at(src, at, h, sizeof(h)) < 8) break;
    uint64_t size = be32(h);
    if (size == 1) size = be64(h + 8);
    else if (size == 0 && src->size > 0) size = (uint64_t)(src->size - at);
    if (size < 8) break;
    if (memcmp(h + 4, "moov", 4) == 0) {
      moov_at = at;
      moov_size = size;
      break;
    }
    if (memcmp(h + 4, "mdat", 4) == 0) mdat_seen = true;
    at += (int64_t)size;
  }
  if (moov_at < 0) {
    snprintf(why, why_len, "M4A without its index (moov)");
    return ESP_ERR_NOT_SUPPORTED;
  }
  if (moov_size > M4A_MOOV_MAX) {
    snprintf(why, why_len, "M4A index of %llu KiB (up to %u KiB)",
             (unsigned long long)(moov_size / 1024U), M4A_MOOV_MAX / 1024U);
    return ESP_ERR_NOT_SUPPORTED;
  }
  uint8_t *moov = ps_alloc((size_t)moov_size);
  if (!moov) {
    snprintf(why, why_len, "Out of memory for the M4A index");
    return ESP_ERR_NO_MEM;
  }
  size_t got = 0;
  while (got < moov_size) {
    const size_t want = moov_size - got > 1048576U ? 1048576U : (size_t)(moov_size - got);
    const int n = src->read_at(src, moov_at + (int64_t)got, moov + got, want);
    if (n <= 0) break;
    got += (size_t)n;
  }
  esp_err_t err = ESP_ERR_NOT_SUPPORTED;
  if (got != moov_size) {
    snprintf(why, why_len, "M4A index could not be read");
  } else {
    size_t pos = 0, blen;
    const uint8_t *type, *body;
    if (next_box(moov, got, &pos, &type, &body, &blen) && parse_moov(t, body, blen, why, why_len))
      err = ESP_OK;
  }
  free(moov);
  if (err != ESP_OK) {
    m4a_close(t);
    return err;
  }
  t->moov_off = moov_at;
  t->moov_last = mdat_seen;
  ESP_LOGI(TAG, "M4A %s: %u samples in %u chunks, largest %u B, timescale %u, %.1f s, moov %s "
           "(%llu KiB at %lld)",
           t->codec == M4A_ALAC ? "ALAC" : t->codec == M4A_AAC ? "AAC" : "MP3",
           (unsigned)t->samples, (unsigned)t->chunks, (unsigned)t->max_size,
           (unsigned)t->timescale, (double)t->duration_ts / t->timescale,
           t->moov_last ? "after the audio" : "first", (unsigned long long)(moov_size / 1024U),
           (long long)moov_at);
  return ESP_OK;
}

void m4a_close(m4a_track_t *t) {
  free(t->sizes);
  free(t->chunk_off);
  free(t->stsc);
  free(t->stts);
  t->sizes = NULL;
  t->chunk_off = NULL;
  t->stsc = NULL;
  t->stts = NULL;
}

static uint32_t sample_size(const m4a_track_t *t, uint32_t i) {
  return t->const_size ? t->const_size : (i < t->samples ? t->sizes[i] : 0);
}

void m4a_iter_seek(const m4a_track_t *t, m4a_iter_t *it, uint32_t i) {
  memset(it, 0, sizeof(*it));
  if (i >= t->samples) {
    it->sample = t->samples;
    return;
  }
  uint32_t first = 0; /* the first sample of the stsc run */
  for (uint32_t k = 0; k < t->n_stsc; ++k) {
    const uint32_t c0 = t->stsc[k].first_chunk - 1;
    const uint32_t c1 = k + 1 < t->n_stsc ? t->stsc[k + 1].first_chunk - 1 : t->chunks;
    const uint64_t in_run = (uint64_t)(c1 > c0 ? c1 - c0 : 0) * t->stsc[k].per_chunk;
    if ((uint64_t)i < first + in_run || k + 1 == t->n_stsc) {
      const uint32_t d = i - first;
      it->stsc_ix = k;
      it->chunk = c0 + d / t->stsc[k].per_chunk;
      it->in_chunk = d % t->stsc[k].per_chunk;
      break;
    }
    first += (uint32_t)in_run;
  }
  it->sample = i;
  it->off = it->chunk < t->chunks ? t->chunk_off[it->chunk] : 0;
  for (uint32_t s = i - it->in_chunk; s < i; ++s) it->off += sample_size(t, s);
}

bool m4a_iter_get(const m4a_track_t *t, const m4a_iter_t *it, uint64_t *off, uint32_t *size) {
  if (it->sample >= t->samples || it->chunk >= t->chunks) return false;
  *off = it->off;
  *size = sample_size(t, it->sample);
  return true;
}

void m4a_iter_next(const m4a_track_t *t, m4a_iter_t *it) {
  it->off += sample_size(t, it->sample);
  it->sample++;
  if (++it->in_chunk >= t->stsc[it->stsc_ix].per_chunk) {
    it->in_chunk = 0;
    it->chunk++;
    if (it->stsc_ix + 1 < t->n_stsc && it->chunk + 1 >= t->stsc[it->stsc_ix + 1].first_chunk)
      it->stsc_ix++;
    if (it->chunk < t->chunks) it->off = t->chunk_off[it->chunk];
  }
}

uint32_t m4a_sample_at(const m4a_track_t *t, uint64_t ts, uint64_t *start_ts) {
  uint64_t at = 0;
  uint32_t i = 0;
  for (uint32_t k = 0; k < t->n_stts; ++k) {
    const uint64_t run = (uint64_t)t->stts[k].count * t->stts[k].delta;
    if (ts < at + run && t->stts[k].delta) {
      const uint32_t d = (uint32_t)((ts - at) / t->stts[k].delta);
      *start_ts = at + (uint64_t)d * t->stts[k].delta;
      return i + d;
    }
    at += run;
    i += t->stts[k].count;
  }
  *start_ts = at;
  return i < t->samples ? i : t->samples;
}

uint32_t m4a_sample_delta(const m4a_track_t *t, uint32_t i) {
  for (uint32_t k = 0; k < t->n_stts; ++k) {
    if (i < t->stts[k].count) return t->stts[k].delta;
    i -= t->stts[k].count;
  }
  return 0;
}

void m4a_adts_header(const m4a_track_t *t, uint32_t len, uint8_t h[7]) {
  const uint32_t fl = len + 7U;
  h[0] = 0xFF;
  h[1] = 0xF1; /* MPEG-4, layer 0, no CRC */
  h[2] = (uint8_t)((t->adts_profile << 6) | (t->adts_rate_ix << 2) | (t->adts_channels >> 2));
  h[3] = (uint8_t)(((t->adts_channels & 3U) << 6) | (fl >> 11));
  h[4] = (uint8_t)(fl >> 3);
  h[5] = (uint8_t)(((fl & 7U) << 5) | 0x1F);
  h[6] = 0xFC;
}
