/* USB player sources and containers (phase 3a). See gw_player_src.h. */

#include "gw_player_src.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"

static const char *TAG = "gw_player_src";

/* ---- HTTP ---- */

/* Connecting and the response's headers may take a while (a NAS that spins
 * its disk up); after that every read waits at most HTTP_READ_TIMEOUT_MS
 * at a time, so an interruption is seen within it. */
#define HTTP_OPEN_TIMEOUT_MS 5000
#define HTTP_READ_TIMEOUT_MS 1000
#define HTTP_READ_TRIES      100 /* the stream: up to 100 s without data, as before */
#define HTTP_SPAN_TRIES      10  /* read_at and the cover: up to 10 s */

typedef struct {
  esp_http_client_handle_t c;
  char *url;
  int64_t pos; /* the file's byte that read gives next */
} http_src_t;

static void http_drop(http_src_t *h) {
  if (h->c) {
    esp_http_client_close(h->c);
    esp_http_client_cleanup(h->c);
  }
  h->c = NULL;
}

/* A GET from byte off on (to last, when last >= off). Status 200 for the
 * whole file, 206 for a part; a 200 to a Range request is
 * ESP_ERR_NOT_SUPPORTED (the server does not seek). */
static esp_err_t http_get(const char *url, int64_t off, int64_t last,
                          esp_http_client_handle_t *out, int64_t *len) {
  const esp_http_client_config_t cfg = {
      .url = url,
      .timeout_ms = HTTP_OPEN_TIMEOUT_MS,
      .buffer_size = 4096,
  };
  esp_http_client_handle_t c = esp_http_client_init(&cfg);
  if (!c) return ESP_ERR_NO_MEM;
  if (off > 0 || last >= 0) {
    char range[48];
    if (last >= off)
      snprintf(range, sizeof(range), "bytes=%lld-%lld", (long long)off, (long long)last);
    else
      snprintf(range, sizeof(range), "bytes=%lld-", (long long)off);
    esp_http_client_set_header(c, "Range", range);
  }
  esp_err_t err = esp_http_client_open(c, 0);
  if (err != ESP_OK) {
    esp_http_client_cleanup(c);
    return err;
  }
  *len = esp_http_client_fetch_headers(c);
  const int status = esp_http_client_get_status_code(c);
  const bool ranged = off > 0 || last >= 0;
  if (status != (ranged ? 206 : 200)) {
    ESP_LOGW(TAG, "HTTP %d for %s%s", status, url, ranged ? " (a Range request)" : "");
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return ranged && status == 200 ? ESP_ERR_NOT_SUPPORTED : ESP_ERR_NOT_FOUND;
  }
  (void)esp_http_client_set_timeout_ms(c, HTTP_READ_TIMEOUT_MS);
  *out = c;
  return ESP_OK;
}

/* One read that waits for data up to tries times HTTP_READ_TIMEOUT_MS:
 * > 0 bytes, 0 at the end, -1 on an error or when nothing came,
 * PLAYER_SRC_INTERRUPTED when s (may be NULL) says so first. */
static int http_read_wait(esp_http_client_handle_t c, uint8_t *buf, size_t len, int tries,
                          const player_source_t *s) {
  for (int k = 0; k < tries; ++k) {
    if (k && s && player_source_interrupted(s)) return PLAYER_SRC_INTERRUPTED;
    const int n = esp_http_client_read(c, (char *)buf, (int)len);
    if (n != -ESP_ERR_HTTP_EAGAIN) return n < 0 ? -1 : n;
  }
  return -1;
}

static int http_read_once(player_source_t *s, http_src_t *h, uint8_t *buf, size_t len) {
  if (!h->c) return -1;
  const int n = http_read_wait(h->c, buf, len, HTTP_READ_TRIES, s);
  if (n == 0 && !esp_http_client_is_complete_data_received(h->c)) return -1;
  return n;
}

static esp_err_t http_seek(player_source_t *s, int64_t off) {
  http_src_t *h = s->ctx;
  esp_http_client_handle_t c = NULL;
  int64_t len = 0;
  const esp_err_t err = http_get(h->url, off, -1, &c, &len);
  if (err != ESP_OK) return err;
  http_drop(h);
  h->c = c;
  h->pos = off;
  return ESP_OK;
}

static int http_read(player_source_t *s, uint8_t *buf, size_t len) {
  http_src_t *h = s->ctx;
  int n = http_read_once(s, h, buf, len);
  if (n == PLAYER_SRC_INTERRUPTED) return n;
  if (n < 0 && h->pos > 0 && (s->size < 0 || h->pos < s->size)) {
    /* the connection broke (the server gave up during a long pause, say):
     * once more from where it was */
    ESP_LOGW(TAG, "HTTP read failed at byte %lld: opening again there", (long long)h->pos);
    for (int k = 0; k < 16 && n < 0; ++k) { /* up to ~8 s: a server that restarts */
      if (player_source_interrupted(s)) return PLAYER_SRC_INTERRUPTED;
      if (k) vTaskDelay(pdMS_TO_TICKS(500));
      if (http_seek(s, h->pos) == ESP_OK) n = http_read_once(s, h, buf, len);
      if (n == PLAYER_SRC_INTERRUPTED) return n;
    }
    if (n < 0) ESP_LOGW(TAG, "HTTP: the file is gone at byte %lld", (long long)h->pos);
  }
  if (n > 0) h->pos += n;
  return n;
}

static int http_read_at(player_source_t *s, int64_t off, uint8_t *buf, size_t len) {
  http_src_t *h = s->ctx;
  if (!len) return 0;
  esp_http_client_handle_t c = NULL;
  int64_t clen = 0;
  const int64_t t0 = esp_timer_get_time();
  if (http_get(h->url, off, off + (int64_t)len - 1, &c, &clen) != ESP_OK) return -1;
  const int64_t t1 = esp_timer_get_time();
  size_t got = 0;
  while (got < len) {
    const int n = http_read_wait(c, buf + got, len - got, HTTP_SPAN_TRIES, s);
    if (n <= 0) break;
    got += (size_t)n;
  }
  const int64_t t2 = esp_timer_get_time();
  esp_http_client_close(c);
  esp_http_client_cleanup(c);
  const int64_t t3 = esp_timer_get_time();
  if (t3 - t0 > 200000)
    ESP_LOGW(TAG, "slow read_at of %u bytes: open %lld ms, read %lld ms, close %lld ms",
             (unsigned)len, (long long)((t1 - t0) / 1000), (long long)((t2 - t1) / 1000),
             (long long)((t3 - t2) / 1000));
  return (int)got;
}

esp_err_t player_http_span(const char *url, int64_t off, uint32_t len,
                           bool (*sink)(void *ctx, const uint8_t *b, size_t n), void *ctx) {
  esp_http_client_handle_t c = NULL;
  int64_t clen = 0;
  if (!len) return ESP_OK;
  const esp_err_t err = http_get(url, off, off + (int64_t)len - 1, &c, &clen);
  if (err != ESP_OK) return err;
  uint8_t *buf = heap_caps_malloc(8192, MALLOC_CAP_SPIRAM);
  esp_err_t r = buf ? ESP_OK : ESP_ERR_NO_MEM;
  uint32_t done = 0;
  while (buf && done < len) {
    const int want = len - done < 8192U ? (int)(len - done) : 8192;
    const int n = http_read_wait(c, buf, (size_t)want, HTTP_SPAN_TRIES, NULL);
    if (n <= 0) { r = ESP_FAIL; break; }
    if (!sink(ctx, buf, (size_t)n)) { r = ESP_FAIL; break; }
    done += (uint32_t)n;
  }
  free(buf);
  esp_http_client_close(c);
  esp_http_client_cleanup(c);
  return r;
}

static void http_close(player_source_t *s) {
  http_src_t *h = s->ctx;
  if (h) {
    http_drop(h);
    free(h->url);
    free(h);
  }
  s->ctx = NULL;
}

esp_err_t player_source_http_open(player_source_t *s, const char *url) {
  memset(s, 0, sizeof(*s));
  http_src_t *h = calloc(1, sizeof(*h));
  if (!h) return ESP_ERR_NO_MEM;
  h->url = strdup(url);
  if (!h->url) {
    free(h);
    return ESP_ERR_NO_MEM;
  }
  int64_t len = 0;
  const esp_err_t err = http_get(url, 0, -1, &h->c, &len);
  if (err != ESP_OK) {
    free(h->url);
    free(h);
    return err;
  }
  s->ctx = h;
  s->read = http_read;
  s->close = http_close;
  s->seek = http_seek;
  s->read_at = http_read_at;
  s->size = len > 0 ? len : -1;
  return ESP_OK;
}

/* 44.1 and 48 kHz, and the hi-res rates the player takes down by 2 or 4
 * (gw_player_decim.c) */
static bool rate_ok(int r) {
  return r == 44100 || r == 48000 || r == 88200 || r == 96000 || r == 176400 || r == 192000;
}

/* ---- a file (the USB drive) ----
 * A reader task reads ahead into a stream buffer in PSRAM, so the USB
 * transfers (the bus waits, not the CPU) run beside the decoder: on the S3's
 * full-speed port a 7.1 ALAC file took as long to read as to decode. read_at
 * has a descriptor of its own, so the stream keeps its place. */

#define FILE_AHEAD_BYTES (1024U * 1024U) /* half of it when PSRAM is short */
#define FILE_CHUNK       (32U * 1024U)
#define FILE_TASK_STACK  8192 /* VFS, FatFs and the USB transfer below read() */
#define FILE_TASK_PRIO   3
#define FILE_TASK_CORE   1
/* at least one tick: the Gateway runs FreeRTOS at 100 Hz, where
 * pdMS_TO_TICKS(5) is 0 and a wait becomes a busy loop */
#define FILE_TICKS(ms) (pdMS_TO_TICKS(ms) ? pdMS_TO_TICKS(ms) : 1)
/* How long a close waits for the reader task; a read() that hangs longer
 * (a drive that stopped answering) leaves the buffers to the reader. */
#define FILE_CLOSE_WAIT_US (5LL * 1000000LL)

typedef struct {
  int fd, fd_at;
  StreamBufferHandle_t sb;
  /* made here, not with xStreamBufferCreateWithCaps: IDF 6.1's
   * vStreamBufferDeleteWithCaps deletes a stream buffer as a queue
   * (vSemaphoreDelete) and then frees it again: a double free */
  StaticStreamBuffer_t *sb_ctl;
  uint8_t *sb_mem;
  TaskHandle_t task;
  SemaphoreHandle_t done;
  uint8_t *chunk;
  volatile uint32_t gen, ack;  /* a seek: gen moves on, the reader answers in ack */
  volatile uint32_t drained;   /* the old bytes are out of the buffer: read on */
  volatile int64_t target;
  volatile bool eof, err, quit;
  /* the source and the reader task: whichever lets go last frees it all */
  atomic_int refs;
} file_src_t;

static void file_free(file_src_t *f) {
  if (f->sb) vStreamBufferDelete(f->sb); /* static: frees nothing */
  free(f->sb_ctl);
  free(f->sb_mem);
  if (f->done) vSemaphoreDelete(f->done);
  free(f->chunk);
  if (f->fd >= 0) close(f->fd);
  if (f->fd_at >= 0) close(f->fd_at);
  free(f);
}

static void file_reader(void *arg) {
  file_src_t *f = arg;
  uint32_t mine = 0; /* gen starts at 0: a seek before this task runs is seen */
  while (!f->quit) {
    const uint32_t g = f->gen;
    if (g != mine) { /* a seek: from the new place */
      f->err = lseek(f->fd, (off_t)f->target, SEEK_SET) != (off_t)f->target;
      f->eof = false;
      mine = g;
      f->ack = g;
      continue;
    }
    if (f->drained != mine) { /* the seek's old bytes are still being thrown away */
      vTaskDelay(FILE_TICKS(1));
      continue;
    }
    if (f->eof || f->err) {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    const ssize_t n = read(f->fd, f->chunk, FILE_CHUNK);
    if (n <= 0) {
      if (n < 0) f->err = true;
      else f->eof = true;
      continue;
    }
    size_t sent = 0;
    while (sent < (size_t)n && !f->quit && f->gen == mine)
      sent += xStreamBufferSend(f->sb, f->chunk + sent, (size_t)n - sent, pdMS_TO_TICKS(20));
  }
  xSemaphoreGive(f->done);
  if (atomic_fetch_sub(&f->refs, 1) == 1) file_free(f); /* the close gave up waiting */
  vTaskDeleteWithCaps(NULL);
}

static int file_read(player_source_t *s, uint8_t *buf, size_t len) {
  file_src_t *f = s->ctx;
  for (;;) {
    const size_t n = xStreamBufferReceive(f->sb, buf, len, pdMS_TO_TICKS(50));
    if (n) return (int)n;
    if (f->err) return -1;
    if (f->eof && xStreamBufferIsEmpty(f->sb)) return 0;
    if (player_source_interrupted(s)) return PLAYER_SRC_INTERRUPTED;
  }
}

/* Drops what was read ahead; the reader goes on from off. Not while another
 * task reads (the player parks the decoder task first). */
static esp_err_t file_seek(player_source_t *s, int64_t off) {
  file_src_t *f = s->ctx;
  f->target = off;
  const uint32_t g = f->gen + 1U;
  f->gen = g;
  uint8_t tmp[256];
  const int64_t until = esp_timer_get_time() + 2000000; /* up to 2 s for the reader */
  while (f->ack != g && esp_timer_get_time() < until) {
    /* make room, so a reader waiting for it sees the seek */
    if (!xStreamBufferReceive(f->sb, tmp, sizeof(tmp), 0)) vTaskDelay(FILE_TICKS(2));
  }
  while (xStreamBufferReceive(f->sb, tmp, sizeof(tmp), 0)) {
  }
  f->drained = g; /* only bytes from off come now */
  if (f->ack != g) ESP_LOGW(TAG, "file seek to %lld: the reader did not answer in 2 s", (long long)off);
  if (f->ack != g) return ESP_ERR_TIMEOUT;
  return f->err ? ESP_FAIL : ESP_OK;
}

static int file_read_at(player_source_t *s, int64_t off, uint8_t *buf, size_t len) {
  file_src_t *f = s->ctx;
  if (lseek(f->fd_at, (off_t)off, SEEK_SET) != (off_t)off) return -1;
  size_t got = 0;
  while (got < len) {
    const ssize_t n = read(f->fd_at, buf + got, len - got);
    if (n <= 0) break;
    got += (size_t)n;
  }
  return (int)got;
}

static void file_close(player_source_t *s) {
  file_src_t *f = s->ctx;
  s->ctx = NULL;
  if (!f) return;
  if (f->task) {
    f->quit = true;
    uint8_t tmp[256];
    const int64_t until = esp_timer_get_time() + FILE_CLOSE_WAIT_US;
    bool done = false;
    while (!(done = xSemaphoreTake(f->done, FILE_TICKS(5)) == pdTRUE) &&
           esp_timer_get_time() < until)
      (void)xStreamBufferReceive(f->sb, tmp, sizeof(tmp), 0);
    if (!done)
      ESP_LOGE(TAG, "the USB reader is still in a read after %lld s: it frees the file when "
                    "the read ends", FILE_CLOSE_WAIT_US / 1000000LL);
  }
  if (atomic_fetch_sub(&f->refs, 1) == 1) file_free(f);
}

esp_err_t player_source_file_open(player_source_t *s, const char *path) {
  memset(s, 0, sizeof(*s));
  file_src_t *f = calloc(1, sizeof(*f));
  if (!f) return ESP_ERR_NO_MEM;
  f->fd = open(path, O_RDONLY);
  f->fd_at = f->fd >= 0 ? open(path, O_RDONLY) : -1;
  struct stat st;
  if (f->fd < 0 || f->fd_at < 0 || fstat(f->fd, &st) != 0) {
    ESP_LOGW(TAG, "%s: errno %d", path, errno);
    if (f->fd >= 0) close(f->fd);
    if (f->fd_at >= 0) close(f->fd_at);
    free(f);
    return ESP_ERR_NOT_FOUND;
  }
  s->ctx = f;
  s->read = file_read;
  s->close = file_close;
  s->seek = file_seek;
  s->read_at = file_read_at;
  s->size = (int64_t)st.st_size;
  f->sb_ctl = heap_caps_malloc(sizeof(StaticStreamBuffer_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  size_t ahead = FILE_AHEAD_BYTES;
  f->sb_mem = heap_caps_malloc(ahead, MALLOC_CAP_SPIRAM);
  if (!f->sb_mem) f->sb_mem = heap_caps_malloc(ahead /= 2U, MALLOC_CAP_SPIRAM);
  if (f->sb_ctl && f->sb_mem) f->sb = xStreamBufferCreateStatic(ahead, 1, f->sb_mem, f->sb_ctl);
  f->chunk = heap_caps_malloc(FILE_CHUNK, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
  if (!f->chunk) f->chunk = heap_caps_malloc(FILE_CHUNK, MALLOC_CAP_SPIRAM);
  f->done = xSemaphoreCreateBinary();
  atomic_init(&f->refs, 2); /* the reader's part, before it can run */
  if (!f->sb || !f->chunk || !f->done ||
      xTaskCreatePinnedToCoreWithCaps(file_reader, "gw_player_rd", FILE_TASK_STACK, f,
                                      FILE_TASK_PRIO, &f->task, FILE_TASK_CORE,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
    f->task = NULL;
    atomic_store(&f->refs, 1); /* no reader */
    file_close(s);
    return ESP_ERR_NO_MEM;
  }
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
  if (!rate_ok(w->rate)) {
    snprintf(w->why, sizeof(w->why), "%d Hz (44.1/48 kHz and their 2x and 4x)", w->rate);
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
    strlcpy(w->why, "Not a WAV, FLAC, MP3, AAC or M4A file", sizeof(w->why));
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

  if (!rate_ok(w->rate)) {
    snprintf(w->why, sizeof(w->why), "%d Hz (44.1/48 kHz and their 2x and 4x)", w->rate);
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

/* ---- MP3 ---- */

bool mp3_parse_first(const uint8_t *b, size_t len, mp3_info_t *m) {
  memset(m, 0, sizeof(*m));
  if (len < 4 || b[0] != 0xFF || (b[1] & 0xE0) != 0xE0) return false;
  const int ver = (b[1] >> 3) & 3; /* 3 MPEG-1, 2 MPEG-2, 0 MPEG-2.5 */
  const int layer = (b[1] >> 1) & 3; /* 1 = layer III */
  const int br_ix = b[2] >> 4, sr_ix = (b[2] >> 2) & 3, pad = (b[2] >> 1) & 1;
  if (ver == 1 || layer != 1 || br_ix == 0 || br_ix == 15 || sr_ix == 3) return false;
  static const int rates[3] = {44100, 48000, 32000};
  static const int16_t kbps1[15] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320};
  static const int16_t kbps2[15] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160};
  const bool mpeg1 = ver == 3;
  m->rate = rates[sr_ix] >> (mpeg1 ? 0 : ver == 2 ? 1 : 2);
  m->channels = (b[3] >> 6) == 3 ? 1 : 2;
  m->spf = mpeg1 ? 1152 : 576;
  m->kbps = mpeg1 ? kbps1[br_ix] : kbps2[br_ix];
  const size_t frame = (size_t)(m->spf / 8) * (size_t)m->kbps * 1000U / (size_t)m->rate + pad;
  const size_t side = mpeg1 ? (m->channels == 1 ? 17U : 32U) : (m->channels == 1 ? 9U : 17U);
  const size_t x = 4 + side;
  if (x + 8 <= len && x + 8 <= frame + 4 &&
      (memcmp(b + x, "Xing", 4) == 0 || memcmp(b + x, "Info", 4) == 0)) {
    const uint32_t flags = ((uint32_t)b[x + 4] << 24) | ((uint32_t)b[x + 5] << 16) |
                           ((uint32_t)b[x + 6] << 8) | b[x + 7];
    size_t at = x + 8;
    if ((flags & 1U) && at + 4 <= len) {
      m->xing_frames = ((uint32_t)b[at] << 24) | ((uint32_t)b[at + 1] << 16) |
                       ((uint32_t)b[at + 2] << 8) | b[at + 3];
      at += 4;
    }
    if ((flags & 2U) && at + 4 <= len) {
      m->xing_bytes = ((uint32_t)b[at] << 24) | ((uint32_t)b[at + 1] << 16) |
                      ((uint32_t)b[at + 2] << 8) | b[at + 3];
      at += 4;
    }
    if ((flags & 4U) && at + 100 <= len) {
      memcpy(m->toc, b + at, 100);
      m->has_toc = true;
    }
    /* the LAME tag comes 120 bytes after "Xing" (fields, TOC, quality) */
    const size_t l = x + 120;
    if (l + 24 <= len && (memcmp(b + l, "LAME", 4) == 0 || memcmp(b + l, "Lavc", 4) == 0 ||
                          memcmp(b + l, "Lavf", 4) == 0)) {
      const uint8_t *d = b + l + 21;
      m->enc_delay = (d[0] << 4) | (d[1] >> 4);
      m->enc_padding = ((d[1] & 0x0F) << 8) | d[2];
      m->lame_ok = true;
    }
  }
  return true;
}

/* ---- FLAC frames, for seeking ---- */

void flac_stream_info(const uint8_t *f, flac_stream_t *out) {
  const uint8_t *si = f + 8;
  memcpy(out->streaminfo, si, sizeof(out->streaminfo));
  out->min_block = ((uint32_t)si[0] << 8) | si[1];
  out->max_block = ((uint32_t)si[2] << 8) | si[3];
  out->max_frame = ((uint32_t)si[7] << 16) | ((uint32_t)si[8] << 8) | si[9];
}

bool flac_frame_crc_ok(const uint8_t *b, size_t len) {
  static uint16_t table[256];
  if (!table[1]) {
    for (uint32_t i = 0; i < 256; ++i) {
      uint16_t c = (uint16_t)(i << 8);
      for (int k = 0; k < 8; ++k) c = (uint16_t)((c & 0x8000) ? (c << 1) ^ 0x8005 : c << 1);
      table[i] = c;
    }
  }
  if (len < 4) return false;
  uint16_t crc = 0;
  for (size_t i = 0; i + 2 < len; ++i) crc = (uint16_t)((crc << 8) ^ table[(crc >> 8) ^ b[i]]);
  return crc == (uint16_t)((b[len - 2] << 8) | b[len - 1]);
}

static uint8_t crc8_07(const uint8_t *b, size_t n) {
  uint8_t c = 0;
  while (n--) {
    c ^= *b++;
    for (int k = 0; k < 8; ++k) c = (uint8_t)((c & 0x80) ? (c << 1) ^ 0x07 : c << 1);
  }
  return c;
}

bool flac_frame_at(const uint8_t *b, size_t len, const flac_stream_t *st, int channels,
                   int bits, uint64_t *first) {
  /* every byte read is checked against len: a header near the end of the
   * file (a short last frame) has fewer than 16 bytes after it */
  if (len < 6 || b[0] != 0xFF || (b[1] & 0xFE) != 0xF8) return false;
  const bool variable = b[1] & 1U;
  const uint8_t bs = b[2] >> 4, rate = b[2] & 0x0F, chan = b[3] >> 4, size = (b[3] >> 1) & 7;
  if (bs == 0 || rate == 15 || (b[3] & 1U) || chan > 10) return false;
  if ((chan <= 7 ? chan + 1 : 2) != channels) return false;
  static const int8_t depth[8] = {0, 8, 12, -1, 16, 20, 24, 32};
  if (depth[size] < 0 || (depth[size] && depth[size] != bits)) return false;
  /* the frame or sample number, UTF-8 style */
  size_t at = 4;
  uint64_t v = b[at];
  int more = 0;
  if (!(v & 0x80)) more = 0;
  else if ((v & 0xE0) == 0xC0) { v &= 0x1F; more = 1; }
  else if ((v & 0xF0) == 0xE0) { v &= 0x0F; more = 2; }
  else if ((v & 0xF8) == 0xF0) { v &= 0x07; more = 3; }
  else if ((v & 0xFC) == 0xF8) { v &= 0x03; more = 4; }
  else if ((v & 0xFE) == 0xFC) { v &= 0x01; more = 5; }
  else if (b[at] == 0xFE) { v = 0; more = 6; }
  else return false;
  at++;
  for (int k = 0; k < more; ++k, ++at) {
    if (at >= len || (b[at] & 0xC0) != 0x80) return false;
    v = (v << 6) | (b[at] & 0x3F);
  }
  if (bs == 6) at += 1;
  else if (bs == 7) at += 2;
  if (rate == 12) at += 1;
  else if (rate == 13 || rate == 14) at += 2;
  if (at + 1 > len || crc8_07(b, at) != b[at]) return false;
  if (variable) {
    *first = v;
  } else {
    if (!st->max_block) return false; /* every block but the last has this size */
    *first = v * st->max_block;
  }
  return true;
}
