/* Tags and cover art (phase 6a). See gw_player_tags.h. */

#include "gw_player_tags.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "gw_player_tags";

#define TAGS_TEXT_MAX 4096U   /* a text frame or comment longer than this is cut */
#define TAGS_VORBIS_MAX 65536U

/* ---- text ---- */

static void put_utf8(char **o, char *end, uint32_t c) {
  char b[4];
  int n;
  if (c < 0x80) { b[0] = (char)c; n = 1; }
  else if (c < 0x800) { b[0] = (char)(0xC0 | (c >> 6)); b[1] = (char)(0x80 | (c & 0x3F)); n = 2; }
  else if (c < 0x10000) { b[0] = (char)(0xE0 | (c >> 12)); b[1] = (char)(0x80 | ((c >> 6) & 0x3F)); b[2] = (char)(0x80 | (c & 0x3F)); n = 3; }
  else { b[0] = (char)(0xF0 | (c >> 18)); b[1] = (char)(0x80 | ((c >> 12) & 0x3F)); b[2] = (char)(0x80 | ((c >> 6) & 0x3F)); b[3] = (char)(0x80 | (c & 0x3F)); n = 4; }
  if (*o + n >= end) return;
  memcpy(*o, b, (size_t)n);
  *o += n;
}

void tags_utf16_to_utf8(const uint8_t *in, size_t len, bool be, char *out, size_t out_len) {
  char *o = out, *end = out + out_len;
  if (!out_len) return;
  for (size_t i = 0; i + 1 < len; i += 2) {
    uint32_t c = be ? (uint32_t)(in[i] << 8 | in[i + 1]) : (uint32_t)(in[i + 1] << 8 | in[i]);
    if (!c) break;
    if (c >= 0xD800 && c < 0xDC00 && i + 3 < len) { /* a surrogate pair */
      const uint32_t lo = be ? (uint32_t)(in[i + 2] << 8 | in[i + 3]) : (uint32_t)(in[i + 3] << 8 | in[i + 2]);
      if (lo >= 0xDC00 && lo < 0xE000) {
        c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
        i += 2;
      }
    }
    put_utf8(&o, end, c);
  }
  *o = '\0';
}

static void latin1_to_utf8(const uint8_t *in, size_t len, char *out, size_t out_len) {
  char *o = out, *end = out + out_len;
  if (!out_len) return;
  for (size_t i = 0; i < len && in[i]; ++i) put_utf8(&o, end, in[i]);
  *o = '\0';
}

static void copy_utf8(const uint8_t *in, size_t len, char *out, size_t out_len) {
  size_t n = 0;
  while (n < len && in[n]) ++n;
  if (n >= out_len) {
    n = out_len - 1;
    while (n > 0 && (in[n] & 0xC0) == 0x80) --n; /* do not cut a letter in two */
  }
  memcpy(out, in, n);
  out[n] = '\0';
}

/* ID3 text: an encoding byte, then the text */
static void id3_text(const uint8_t *b, size_t len, char *out, size_t out_len) {
  if (!len) return;
  const uint8_t enc = b[0];
  b++;
  len--;
  if (enc == 0) latin1_to_utf8(b, len, out, out_len);
  else if (enc == 3) copy_utf8(b, len, out, out_len);
  else if (enc == 1) { /* UTF-16 with a BOM */
    bool be = false;
    if (len >= 2 && b[0] == 0xFE && b[1] == 0xFF) { be = true; b += 2; len -= 2; }
    else if (len >= 2 && b[0] == 0xFF && b[1] == 0xFE) { b += 2; len -= 2; }
    tags_utf16_to_utf8(b, len, be, out, out_len);
  } else if (enc == 2) {
    tags_utf16_to_utf8(b, len, true, out, out_len);
  }
}

/* ---- ID3v2 ---- */

static uint32_t syncsafe(const uint8_t *p) {
  return ((uint32_t)(p[0] & 0x7F) << 21) | ((uint32_t)(p[1] & 0x7F) << 14) |
         ((uint32_t)(p[2] & 0x7F) << 7) | (p[3] & 0x7F);
}
static uint32_t be32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

void tags_read_id3(player_source_t *src, int64_t tag_bytes, player_tags_t *t) {
  uint8_t h[10];
  if (!src->read_at || tag_bytes < 10 || src->read_at(src, 0, h, 10) != 10 || memcmp(h, "ID3", 3) != 0)
    return;
  const int ver = h[3];
  const bool unsync = h[5] & 0x80;
  if (ver != 3 && ver != 4) return; /* ID3v2.2 is not read */
  int64_t at = 10;
  if (h[5] & 0x40) { /* an extended header */
    uint8_t e[4];
    if (src->read_at(src, at, e, 4) != 4) return;
    at += ver == 4 ? syncsafe(e) : 4 + be32(e);
  }
  uint8_t *buf = heap_caps_malloc(TAGS_TEXT_MAX, MALLOC_CAP_SPIRAM);
  if (!buf) return;
  for (int k = 0; k < 200 && at + 10 <= tag_bytes; ++k) {
    uint8_t f[10];
    if (src->read_at(src, at, f, 10) != 10 || f[0] == 0) break; /* padding */
    const uint32_t size = ver == 4 ? syncsafe(f + 4) : be32(f + 4);
    const int64_t body = at + 10;
    if (size == 0 || body + size > tag_bytes) break;
    const bool text = f[0] == 'T';
    char *dst = NULL;
    size_t dlen = 0;
    if (!memcmp(f, "TIT2", 4)) { dst = t->title; dlen = sizeof(t->title); }
    else if (!memcmp(f, "TPE1", 4)) { dst = t->artist; dlen = sizeof(t->artist); }
    else if (!memcmp(f, "TALB", 4)) { dst = t->album; dlen = sizeof(t->album); }
    else if (!memcmp(f, "TRCK", 4)) { dst = t->track; dlen = sizeof(t->track); }
    else if (!memcmp(f, "TYER", 4) || !memcmp(f, "TDRC", 4)) { dst = t->year; dlen = sizeof(t->year); }
    if (text && dst && !dst[0]) {
      const size_t n = size < TAGS_TEXT_MAX ? size : TAGS_TEXT_MAX;
      if (src->read_at(src, body, buf, n) == (int)n) id3_text(buf, n, dst, dlen);
    } else if (!memcmp(f, "APIC", 4) && !t->cover_len && !unsync && !(f[9] & 0x0E)) {
      /* encoding, MIME (Latin-1, NUL), picture type, description (NUL), data */
      const size_t n = size < 512U ? size : 512U;
      if (src->read_at(src, body, buf, n) == (int)n) {
        const uint8_t enc = buf[0];
        size_t i = 1;
        char mime[24] = "";
        size_t m = 0;
        while (i < n && buf[i]) { if (m + 1 < sizeof(mime)) mime[m++] = (char)buf[i]; ++i; }
        mime[m] = '\0';
        i += 2; /* the NUL, the picture type */
        if (enc == 1 || enc == 2) {
          while (i + 1 < n && (buf[i] || buf[i + 1])) i += 2;
          i += 2;
        } else {
          while (i < n && buf[i]) ++i;
          i += 1;
        }
        if (i < n && i < size) {
          t->cover_off = body + (int64_t)i;
          t->cover_len = size - (uint32_t)i;
          strlcpy(t->cover_mime, strchr(mime, '/') ? mime : !strcasecmp(mime, "PNG") ? "image/png" : "image/jpeg",
                  sizeof(t->cover_mime));
        }
      }
    }
    at = body + size;
  }
  free(buf);
}

/* ---- FLAC ---- */

void tags_vorbis(const uint8_t *b, size_t len, player_tags_t *t) {
  if (len < 8) return;
  uint32_t vl = b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24; /* the vendor string */
  size_t at = 4 + (size_t)vl;
  if (at + 4 > len) return;
  const uint32_t n = b[at] | b[at + 1] << 8 | b[at + 2] << 16 | (uint32_t)b[at + 3] << 24;
  at += 4;
  for (uint32_t k = 0; k < n && at + 4 <= len; ++k) {
    const uint32_t cl = b[at] | b[at + 1] << 8 | b[at + 2] << 16 | (uint32_t)b[at + 3] << 24;
    at += 4;
    if (cl > len - at) break;
    const char *c = (const char *)b + at;
    const char *eq = memchr(c, '=', cl);
    if (eq) {
      const size_t kl = (size_t)(eq - c);
      const uint8_t *v = (const uint8_t *)eq + 1;
      const size_t vlen = cl - kl - 1;
      char *dst = NULL;
      size_t dlen = 0;
      if (kl == 5 && !strncasecmp(c, "TITLE", 5)) { dst = t->title; dlen = sizeof(t->title); }
      else if (kl == 6 && !strncasecmp(c, "ARTIST", 6)) { dst = t->artist; dlen = sizeof(t->artist); }
      else if (kl == 5 && !strncasecmp(c, "ALBUM", 5)) { dst = t->album; dlen = sizeof(t->album); }
      else if (kl == 11 && !strncasecmp(c, "TRACKNUMBER", 11)) { dst = t->track; dlen = sizeof(t->track); }
      else if (kl == 4 && !strncasecmp(c, "DATE", 4)) { dst = t->year; dlen = sizeof(t->year); }
      if (dst && !dst[0]) copy_utf8(v, vlen, dst, dlen);
    }
    at += cl;
  }
}

void tags_read_flac(player_source_t *src, int64_t flac_at, int64_t audio_start, player_tags_t *t) {
  if (!src->read_at) return;
  int64_t at = flac_at + 4;
  for (int k = 0; k < 128 && at + 4 <= audio_start; ++k) {
    uint8_t h[4];
    if (src->read_at(src, at, h, 4) != 4) return;
    const uint32_t len = ((uint32_t)h[1] << 16) | ((uint32_t)h[2] << 8) | h[3];
    const int type = h[0] & 0x7F;
    const int64_t body = at + 4;
    if (type == 4 && len <= TAGS_VORBIS_MAX) {
      uint8_t *b = heap_caps_malloc(len ? len : 1, MALLOC_CAP_SPIRAM);
      if (b && src->read_at(src, body, b, len) == (int)len) tags_vorbis(b, len, t);
      free(b);
    } else if (type == 6 && len >= 32) {
      /* picture type, MIME, description, width, height, depth, colours, data */
      uint8_t p[8 + 64];
      const size_t n = len < sizeof(p) ? len : sizeof(p);
      if (src->read_at(src, body, p, n) == (int)n) {
        const uint32_t ptype = be32(p), ml = be32(p + 4);
        if (ml < 64 && 8 + ml + 4 <= len) {
          char mime[24];
          const size_t mc = ml < sizeof(mime) - 1 ? ml : sizeof(mime) - 1;
          uint8_t mb[64], dl4[4];
          if (src->read_at(src, body + 8, mb, ml) == (int)ml) {
            memcpy(mime, mb, mc);
            mime[mc] = '\0';
            if (src->read_at(src, body + 8 + ml, dl4, 4) == 4) {
              const uint32_t dlen = be32(dl4);
              const int64_t data_len_at = body + 8 + ml + 4 + dlen + 16;
              uint8_t sz[4];
              if (data_len_at + 4 <= body + len && src->read_at(src, data_len_at, sz, 4) == 4) {
                const uint32_t plen = be32(sz);
                /* a front cover (3) wins over any other picture */
                if (plen && data_len_at + 4 + plen <= body + len && (!t->cover_len || ptype == 3)) {
                  t->cover_off = data_len_at + 4;
                  t->cover_len = plen;
                  strlcpy(t->cover_mime, mime[0] ? mime : "image/jpeg", sizeof(t->cover_mime));
                }
              }
            }
          }
        }
      }
    }
    at = body + len;
    if (h[0] & 0x80) break;
  }
  if (t->title[0] || t->cover_len)
    ESP_LOGI(TAG, "FLAC tags: \"%s\" by \"%s\", cover %u B", t->title, t->artist, (unsigned)t->cover_len);
}
