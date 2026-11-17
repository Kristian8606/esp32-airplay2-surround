/* USB player, phase 0: measurements only (CONFIG_GATEWAY_PLAYER_BENCH,
 * off by default). Nothing here changes what the Gateway does: one task
 * runs once, some seconds after boot, logs its results with the tag
 * "BENCH" and ends. See docs/player-design.md.
 *
 * ALAC encoder (esp_alac_enc of esp_audio_codec), mono, frames of 352
 * samples like the radio:
 *   - time per frame and share of a core at 44.1/16, 48/16 and 48/24, with
 *     fast_mode on and off, for a music-like signal and for white noise
 *     (the worst case);
 *   - the encoded size (Mbit/s of payload and on the wire to a speaker);
 *   - the ALACSpecificConfig ("cookie") the encoder gives;
 *   - bytesShifted and the escape flag of every frame header;
 *   - a decode round trip with esp_alac_dec, once with the encoder's cookie
 *     and once with the cookie the speakers build today (40/10/14), checked
 *     sample by sample against the input;
 *   - eight mono encoders at 48/24 in turn, as the player would run them
 *     for a 7.1 file.
 * The synthetic music compresses better than real music; the noise run is
 * the upper bound of size and time. */

#include "gw_bench.h"
#include "gw_bench_alac_lite.h"

#include <inttypes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "decoder/impl/esp_alac_dec.h"
#include "esp_alac_enc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "BENCH";

#define BENCH_FRAME     352U
#define BENCH_SECONDS   5U
#define BENCH_LANES     8U
#define BENCH_WIRE_EXTRA (2U + 12U + 16U + 8U) /* len, RTP, tag, nonce */
#define BENCH_START_DELAY_MS 20000
#define BENCH_CORE      0
#define BENCH_PRIORITY  3 /* the radio's */

typedef enum {
  SIG_MUSIC = 0, /* tones + coloured noise + a noise floor */
  SIG_NOISE,     /* white noise near full scale: hardly compressible */
  SIG_MUSIC16,   /* music at 16 bit, moved 8 bits up (the radio's 24 bit) */
} sig_t;

static const char *sig_name(sig_t s) {
  return s == SIG_MUSIC ? "music" : s == SIG_NOISE ? "noise" : "music16<<8";
}

/* Float only (the S3 has no double FPU) and phases kept in [0, 2pi), so
 * sinf stays cheap: making the signal must not take longer than the test. */
#define GEN_TONES 5
static const float k_tone_hz[GEN_TONES] = {0.3f, 110.0f, 440.0f, 1318.5f, 5274.0f};

typedef struct {
  uint32_t rng;
  float lp;    /* coloured noise state */
  float phase; /* per lane */
  float ph[GEN_TONES];
} gen_t;

static float rnd(gen_t *g) { /* -1..1 */
  g->rng = g->rng * 1664525U + 1013904223U;
  return (float)(int32_t)g->rng / 2147483648.0f;
}

/* One sample, full scale = 1.0. */
static float gen_next(gen_t *g, sig_t s, int rate) {
  if (s == SIG_NOISE) return 0.5f * rnd(g);
  const float two_pi = 6.28318531f;
  float x[GEN_TONES];
  for (int k = 0; k < GEN_TONES; ++k) {
    x[k] = sinf(g->ph[k] + (k == 2 || k == 4 ? 0.0f : (k == 3 ? 2.0f : 1.0f) * g->phase));
    g->ph[k] += two_pi * k_tone_hz[k] / (float)rate;
    if (g->ph[k] >= two_pi) g->ph[k] -= two_pi;
  }
  const float env = 0.6f + 0.4f * x[0];
  float v = env * (0.12f * x[1] + 0.08f * x[2] + 0.05f * x[3] + 0.03f * x[4]);
  g->lp = 0.97f * g->lp + 0.03f * rnd(g);
  v += 0.25f * g->lp;    /* coloured noise, about -30 dBFS */
  v += 1.0e-5f * rnd(g); /* noise floor, -100 dBFS */
  return v;
}

static int32_t to_int(float v, int bits) {
  const float full = bits == 24 ? 8388607.0f : 32767.0f;
  float x = v * full;
  if (x > full) x = full;
  if (x < -full - 1.0f) x = -full - 1.0f;
  return (int32_t)lrintf(x);
}

/* One frame of PCM in the encoder's layout: int16 LE, or 24 bit packed LE. */
static void fill_frame(gen_t *g, sig_t s, int rate, int bits, uint8_t *dst) {
  for (uint32_t i = 0; i < BENCH_FRAME; ++i) {
    const float v = gen_next(g, s, rate);
    if (bits == 16) {
      const int16_t x = (int16_t)to_int(v, 16);
      memcpy(dst + i * 2U, &x, 2);
    } else {
      const int32_t x = s == SIG_MUSIC16 ? (to_int(v, 16) * 256) : to_int(v, 24);
      dst[i * 3U] = (uint8_t)x;
      dst[i * 3U + 1U] = (uint8_t)(x >> 8);
      dst[i * 3U + 2U] = (uint8_t)(x >> 16);
    }
  }
}

static uint32_t be32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void log_cookie(const char *what, const uint8_t *info, uint32_t len) {
  if (!info || len < 24U) {
    ESP_LOGW(TAG, "%s: no cookie (len %" PRIu32 ")", what, len);
    return;
  }
  const uint8_t *c = info + (len - 24U);
  ESP_LOGI(TAG,
           "%s: cookie len %" PRIu32 " | frameLength %" PRIu32 " compat %u bitDepth %u "
           "pb/mb/kb %u/%u/%u channels %u maxRun %u maxFrameBytes %" PRIu32
           " avgBitRate %" PRIu32 " sampleRate %" PRIu32,
           what, len, be32(c), c[4], c[5], c[6], c[7], c[8], c[9],
           (unsigned)((c[10] << 8) | c[11]), be32(c + 12), be32(c + 16), be32(c + 20));
  if (len > 24U) {
    char hex[3 * 24 + 1] = "";
    const uint32_t n = len - 24U < 24U ? len - 24U : 24U;
    for (uint32_t i = 0; i < n; ++i) snprintf(hex + 3 * i, 4, "%02x ", info[i]);
    ESP_LOGI(TAG, "%s: bytes before the 24-byte config: %s", what, hex);
  }
}

/* The speakers' cookie today (alac_decoder.c make_cookie), for 1 channel. */
static void speaker_cookie(uint8_t c[24], int rate, int bits) {
  memset(c, 0, 24);
  c[0] = 0; c[1] = 0; c[2] = (uint8_t)(BENCH_FRAME >> 8); c[3] = (uint8_t)BENCH_FRAME;
  c[5] = (uint8_t)bits;
  c[6] = 40; c[7] = 10; c[8] = 14;
  c[9] = 1;
  c[10] = 0; c[11] = 255;
  c[20] = (uint8_t)(rate >> 24); c[21] = (uint8_t)(rate >> 16);
  c[22] = (uint8_t)(rate >> 8); c[23] = (uint8_t)rate;
}

typedef struct {
  void *h;
  uint32_t errors, mismatches, frames;
} dec_check_t;

static bool dec_open(dec_check_t *d, const uint8_t *cookie, uint32_t len) {
  memset(d, 0, sizeof(*d));
  esp_alac_dec_cfg_t cfg = ESP_ALAC_DEC_CONFIG_DEFAULT();
  cfg.codec_spec_info = (uint8_t *)cookie;
  cfg.spec_info_len = len;
  return esp_alac_dec_open(&cfg, sizeof(cfg), &d->h) == ESP_AUDIO_ERR_OK;
}

static void dec_check(dec_check_t *d, uint8_t *enc, uint32_t enc_len,
                      const uint8_t *pcm, uint32_t pcm_len, uint8_t *scratch,
                      uint32_t scratch_len) {
  if (!d->h) return;
  d->frames++;
  esp_audio_dec_in_raw_t raw = {.buffer = enc, .len = enc_len};
  esp_audio_dec_out_frame_t out = {.buffer = scratch, .len = scratch_len};
  esp_audio_dec_info_t info = {0};
  if (esp_alac_dec_decode(d->h, &raw, &out, &info) != ESP_AUDIO_ERR_OK) {
    d->errors++;
    esp_alac_dec_reset(d->h);
    return;
  }
  if (out.decoded_size != pcm_len || memcmp(scratch, pcm, pcm_len) != 0) d->mismatches++;
}

static void dec_close(dec_check_t *d) {
  if (d->h) esp_alac_dec_close(d->h);
  d->h = NULL;
}

static bool enc_open(void **h, int rate, int bits, bool fast, int *in_size, int *out_size) {
  esp_alac_enc_config_t ec = ESP_ALAC_ENC_CONFIG_DEFAULT();
  ec.sample_rate = rate;
  ec.channel = 1;
  ec.bits_per_sample = (uint8_t)bits;
  ec.fast_mode = fast;
  ec.frame_samples = BENCH_FRAME;
  if (esp_alac_enc_open(&ec, sizeof(ec), h) != ESP_AUDIO_ERR_OK) return false;
  esp_alac_enc_get_frame_size(*h, in_size, out_size);
  return true;
}

/* One mono encoder over BENCH_SECONDS of the signal. */
static void run_one(int rate, int bits, bool fast, sig_t s) {
  const size_t int_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  const size_t ps_before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  void *enc = NULL;
  int in_size = 0, out_size = 0;
  if (!enc_open(&enc, rate, bits, fast, &in_size, &out_size)) {
    ESP_LOGE(TAG, "ALAC %d/%d fast=%d: open failed", rate, bits, fast);
    return;
  }
  const size_t int_used = int_before - heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  const size_t ps_used = ps_before - heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  const uint32_t pcm_len = BENCH_FRAME * (uint32_t)(bits / 8);
  uint8_t *pcm = heap_caps_malloc((size_t)in_size > pcm_len ? (size_t)in_size : pcm_len,
                                  MALLOC_CAP_SPIRAM);
  uint8_t *out = heap_caps_malloc((size_t)out_size, MALLOC_CAP_SPIRAM);
  uint8_t *scratch = heap_caps_malloc(pcm_len * 2U, MALLOC_CAP_SPIRAM);
  if (!pcm || !out || !scratch) {
    ESP_LOGE(TAG, "out of memory");
    goto done;
  }
  if (in_size != (int)pcm_len)
    ESP_LOGW(TAG, "encoder wants %d input bytes per frame, expected %" PRIu32, in_size, pcm_len);

  esp_audio_enc_info_t ei = {0};
  esp_alac_enc_get_info(enc, &ei);
  char what[64];
  snprintf(what, sizeof(what), "ALAC %d/%d mono fast=%d", rate, bits, fast ? 1 : 0);
  if (s == SIG_MUSIC) log_cookie(what, ei.codec_spec_info, ei.spec_info_len);

  dec_check_t own, spk;
  const bool own_ok = ei.codec_spec_info && dec_open(&own, ei.codec_spec_info, ei.spec_info_len);
  uint8_t sc[24];
  speaker_cookie(sc, rate, bits);
  const bool spk_ok = dec_open(&spk, sc, sizeof(sc));

  gen_t g = {.rng = 12345U};
  const uint32_t frames = BENCH_SECONDS * (uint32_t)rate / BENCH_FRAME;
  int64_t sum_us = 0, max_us = 0;
  uint64_t bytes = 0;
  uint32_t shift_hist[4] = {0}, escapes = 0, enc_err = 0, max_bytes = 0;
  for (uint32_t f = 0; f < frames; ++f) {
    fill_frame(&g, s, rate, bits, pcm);
    esp_audio_enc_in_frame_t in = {.buffer = pcm, .len = (uint32_t)in_size};
    esp_audio_enc_out_frame_t o = {.buffer = out, .len = (uint32_t)out_size};
    const int64_t t0 = esp_timer_get_time();
    const esp_audio_err_t r = esp_alac_enc_process(enc, &in, &o);
    const int64_t dt = esp_timer_get_time() - t0;
    if (r != ESP_AUDIO_ERR_OK || o.encoded_bytes == 0) {
      enc_err++;
      continue;
    }
    sum_us += dt;
    if (dt > max_us) max_us = dt;
    bytes += o.encoded_bytes;
    if (o.encoded_bytes > max_bytes) max_bytes = o.encoded_bytes;
    if (o.encoded_bytes >= 3U) {
      shift_hist[(out[2] >> 2) & 3U]++;
      if (out[2] & 0x02U) escapes++;
    }
    dec_check(&own, out, o.encoded_bytes, pcm, pcm_len, scratch, pcm_len * 2U);
    dec_check(&spk, out, o.encoded_bytes, pcm, pcm_len, scratch, pcm_len * 2U);
    if ((f & 63U) == 63U) vTaskDelay(1); /* let the idle task run */
  }
  const uint32_t ok = frames - enc_err;
  if (ok) {
    const double period_us = 1e6 * BENCH_FRAME / rate;
    const double avg = (double)sum_us / ok;
    const double secs = (double)ok * BENCH_FRAME / rate;
    const double mbit = (double)bytes * 8.0 / secs / 1e6;
    const double wire = ((double)bytes + (double)ok * BENCH_WIRE_EXTRA) * 8.0 / secs / 1e6;
    const double raw_mbit = (double)rate * bits / 1e6;
    ESP_LOGI(TAG,
             "%s %s: %.0f us avg, %lld us max per frame of %.0f us = %.2f%% of a core | "
             "%.3f Mbit/s payload (%.0f%% of PCM %.3f), %.3f on the wire, max frame %" PRIu32
             " B | bytesShifted 0/1/2: %" PRIu32 "/%" PRIu32 "/%" PRIu32 " escape %" PRIu32
             " | enc errors %" PRIu32 " | heap internal %u B, PSRAM %u B",
             what, sig_name(s), avg, (long long)max_us, period_us, 100.0 * avg / period_us,
             mbit, 100.0 * mbit / raw_mbit, raw_mbit, wire, max_bytes, shift_hist[0],
             shift_hist[1], shift_hist[2], escapes, enc_err, (unsigned)int_used,
             (unsigned)ps_used);
  }
  ESP_LOGI(TAG, "%s %s: decode with own cookie: %s, %" PRIu32 " errors, %" PRIu32
           " not bit-exact of %" PRIu32 " | with the speakers' 40/10/14 cookie: %s, %" PRIu32
           " errors, %" PRIu32 " not bit-exact",
           what, sig_name(s), own_ok ? "opened" : "NOT OPENED", own.errors, own.mismatches,
           own.frames, spk_ok ? "opened" : "NOT OPENED", spk.errors, spk.mismatches);
  if (own_ok) dec_close(&own);
  if (spk_ok) dec_close(&spk);
done:
  free(pcm);
  free(out);
  free(scratch);
  esp_alac_enc_close(enc);
}

/* Eight mono encoders in turn, one frame each per 352-sample period. */
static void run_lanes(int rate, int bits, bool fast) {
  void *enc[BENCH_LANES] = {0};
  gen_t g[BENCH_LANES];
  int in_size = 0, out_size = 0;
  uint8_t *pcm = NULL, *out = NULL;
  for (uint32_t i = 0; i < BENCH_LANES; ++i) {
    if (!enc_open(&enc[i], rate, bits, fast, &in_size, &out_size)) {
      ESP_LOGE(TAG, "lanes: open %" PRIu32 " failed", i);
      goto done;
    }
    g[i] = (gen_t){.rng = 777U + i * 9973U, .phase = 0.7f * (float)i};
  }
  pcm = heap_caps_malloc((size_t)in_size, MALLOC_CAP_SPIRAM);
  out = heap_caps_malloc((size_t)out_size, MALLOC_CAP_SPIRAM);
  if (!pcm || !out) goto done;
  const uint32_t frames = BENCH_SECONDS * (uint32_t)rate / BENCH_FRAME;
  int64_t sum_us = 0, max_us = 0;
  uint64_t bytes = 0;
  for (uint32_t f = 0; f < frames; ++f) {
    int64_t period = 0;
    for (uint32_t i = 0; i < BENCH_LANES; ++i) {
      fill_frame(&g[i], SIG_MUSIC, rate, bits, pcm);
      esp_audio_enc_in_frame_t in = {.buffer = pcm, .len = (uint32_t)in_size};
      esp_audio_enc_out_frame_t o = {.buffer = out, .len = (uint32_t)out_size};
      const int64_t t0 = esp_timer_get_time();
      if (esp_alac_enc_process(enc[i], &in, &o) == ESP_AUDIO_ERR_OK) bytes += o.encoded_bytes;
      period += esp_timer_get_time() - t0;
    }
    sum_us += period;
    if (period > max_us) max_us = period;
    if ((f & 15U) == 15U) vTaskDelay(1);
  }
  const double period_us = 1e6 * BENCH_FRAME / rate;
  const double avg = (double)sum_us / frames;
  const double secs = (double)frames * BENCH_FRAME / rate;
  ESP_LOGI(TAG,
           "ALAC %d/%d x%u mono lanes fast=%d music: %.0f us avg, %lld us max per period of "
           "%.0f us = %.1f%% of a core | %.2f Mbit/s payload all lanes, %.2f on the wire",
           rate, bits, (unsigned)BENCH_LANES, fast ? 1 : 0, avg, (long long)max_us, period_us,
           100.0 * avg / period_us, (double)bytes * 8.0 / secs / 1e6,
           ((double)bytes + (double)frames * BENCH_LANES * BENCH_WIRE_EXTRA) * 8.0 / secs / 1e6);
done:
  for (uint32_t i = 0; i < BENCH_LANES; ++i)
    if (enc[i]) esp_alac_enc_close(enc[i]);
  free(pcm);
  free(out);
}

/* Packed frame (int16 LE or 24-bit LE) to sign-extended int32. */
static void unpack(const uint8_t *src, int bits, int32_t *dst) {
  for (uint32_t i = 0; i < BENCH_FRAME; ++i) {
    if (bits == 16) {
      int16_t v;
      memcpy(&v, src + i * 2U, 2);
      dst[i] = v;
    } else {
      const uint8_t *b = src + i * 3U;
      dst[i] = (int32_t)(((uint32_t)b[0] << 8) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 24)) >> 8;
    }
  }
}

/* The light encoder (phase 0b) over the same signal, checked by the same
 * decoder with the speakers' cookie. */
static void run_lite(int rate, int bits, int order, sig_t s) {
  const uint32_t pcm_len = BENCH_FRAME * (uint32_t)(bits / 8);
  const size_t out_cap = pcm_len + 64U;
  uint8_t *pcm = heap_caps_malloc(pcm_len, MALLOC_CAP_SPIRAM);
  uint8_t *out = heap_caps_malloc(out_cap, MALLOC_CAP_SPIRAM);
  uint8_t *scratch = heap_caps_malloc(pcm_len * 2U, MALLOC_CAP_SPIRAM);
  static int32_t x[BENCH_FRAME];
  alac_lite_t e;
  alac_lite_init(&e, bits, order);
  dec_check_t spk = {0};
  uint8_t sc[24];
  speaker_cookie(sc, rate, bits);
  const bool spk_ok = pcm && out && scratch && dec_open(&spk, sc, sizeof(sc));
  if (!spk_ok) {
    ESP_LOGE(TAG, "lite: out of memory or decoder not opened");
    goto done;
  }
  gen_t g = {.rng = 12345U};
  const uint32_t frames = BENCH_SECONDS * (uint32_t)rate / BENCH_FRAME;
  int64_t sum_us = 0, max_us = 0;
  uint64_t bytes = 0;
  uint32_t escapes = 0, enc_err = 0;
  for (uint32_t f = 0; f < frames; ++f) {
    fill_frame(&g, s, rate, bits, pcm);
    unpack(pcm, bits, x);
    bool esc = false;
    const int64_t t0 = esp_timer_get_time();
    const size_t n = alac_lite_encode(&e, x, BENCH_FRAME, out, out_cap, &esc);
    const int64_t dt = esp_timer_get_time() - t0;
    if (!n) {
      enc_err++;
      continue;
    }
    sum_us += dt;
    if (dt > max_us) max_us = dt;
    bytes += n;
    if (esc) escapes++;
    dec_check(&spk, out, (uint32_t)n, pcm, pcm_len, scratch, pcm_len * 2U);
    if ((f & 63U) == 63U) vTaskDelay(1);
  }
  const uint32_t ok = frames - enc_err;
  if (ok) {
    const double period_us = 1e6 * BENCH_FRAME / rate;
    const double avg = (double)sum_us / ok;
    const double secs = (double)ok * BENCH_FRAME / rate;
    const double mbit = (double)bytes * 8.0 / secs / 1e6;
    ESP_LOGI(TAG,
             "LITE %d/%d mono order %d %s: %.0f us avg, %lld us max per frame of %.0f us = "
             "%.2f%% of a core | %.3f Mbit/s payload (%.0f%% of PCM), %.3f on the wire | "
             "escape %" PRIu32 " | enc errors %" PRIu32 " | speakers' decoder: %" PRIu32
             " errors, %" PRIu32 " not bit-exact of %" PRIu32,
             rate, bits, order, sig_name(s), avg, (long long)max_us, period_us,
             100.0 * avg / period_us, mbit, 100.0 * mbit / ((double)rate * bits / 1e6),
             ((double)bytes + (double)ok * BENCH_WIRE_EXTRA) * 8.0 / secs / 1e6, escapes,
             enc_err, spk.errors, spk.mismatches, spk.frames);
  }
done:
  dec_close(&spk);
  free(pcm);
  free(out);
  free(scratch);
}

static void run_lanes_lite(int rate, int bits, int order) {
  const uint32_t pcm_len = BENCH_FRAME * (uint32_t)(bits / 8);
  const size_t out_cap = pcm_len + 64U;
  uint8_t *pcm = heap_caps_malloc(pcm_len, MALLOC_CAP_SPIRAM);
  uint8_t *out = heap_caps_malloc(out_cap, MALLOC_CAP_SPIRAM);
  static int32_t x[BENCH_FRAME];
  alac_lite_t e[BENCH_LANES];
  gen_t g[BENCH_LANES];
  if (!pcm || !out) goto done;
  for (uint32_t i = 0; i < BENCH_LANES; ++i) {
    alac_lite_init(&e[i], bits, order);
    g[i] = (gen_t){.rng = 777U + i * 9973U, .phase = 0.7f * (float)i};
  }
  const uint32_t frames = BENCH_SECONDS * (uint32_t)rate / BENCH_FRAME;
  int64_t sum_us = 0, max_us = 0;
  uint64_t bytes = 0;
  for (uint32_t f = 0; f < frames; ++f) {
    int64_t period = 0;
    for (uint32_t i = 0; i < BENCH_LANES; ++i) {
      fill_frame(&g[i], SIG_MUSIC, rate, bits, pcm);
      unpack(pcm, bits, x);
      const int64_t t0 = esp_timer_get_time();
      bytes += alac_lite_encode(&e[i], x, BENCH_FRAME, out, out_cap, NULL);
      period += esp_timer_get_time() - t0;
    }
    sum_us += period;
    if (period > max_us) max_us = period;
    if ((f & 15U) == 15U) vTaskDelay(1);
  }
  const double period_us = 1e6 * BENCH_FRAME / rate;
  const double avg = (double)sum_us / frames;
  const double secs = (double)frames * BENCH_FRAME / rate;
  ESP_LOGI(TAG,
           "LITE %d/%d x%u mono lanes order %d music: %.0f us avg, %lld us max per period of "
           "%.0f us = %.1f%% of a core | %.2f Mbit/s payload all lanes, %.2f on the wire",
           rate, bits, (unsigned)BENCH_LANES, order, avg, (long long)max_us, period_us,
           100.0 * avg / period_us, (double)bytes * 8.0 / secs / 1e6,
           ((double)bytes + (double)frames * BENCH_LANES * BENCH_WIRE_EXTRA) * 8.0 / secs / 1e6);
done:
  free(pcm);
  free(out);
}

static void bench_task(void *arg) {
  (void)arg;
  vTaskDelay(pdMS_TO_TICKS(BENCH_START_DELAY_MS));
  ESP_LOGI(TAG, "phase 0b bench start: CPU %d MHz, core %d, priority %d, %u s per run",
           CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ, xPortGetCoreID(), BENCH_PRIORITY,
           (unsigned)BENCH_SECONDS);
  static const struct { int rate, bits; } fmts[] = {{48000, 24}, {44100, 16}, {48000, 16}};
  /* esp_alac_enc once per format (fast_mode made no difference), then the
   * light encoder (phase 0b) with order 4 and 8 on the same signals. */
  for (size_t i = 0; i < sizeof(fmts) / sizeof(fmts[0]); ++i) {
    run_one(fmts[i].rate, fmts[i].bits, true, SIG_MUSIC);
    for (int order = 4; order <= 8; order += 4) {
      run_lite(fmts[i].rate, fmts[i].bits, order, SIG_MUSIC);
      run_lite(fmts[i].rate, fmts[i].bits, order, SIG_NOISE);
    }
  }
  run_lite(48000, 24, 8, SIG_MUSIC16);
  run_lanes(48000, 24, true);
  run_lanes_lite(48000, 24, 4);
  run_lanes_lite(48000, 24, 8);
  run_lanes_lite(44100, 16, 8);
  ESP_LOGI(TAG, "phase 0b bench done");
  vTaskDelete(NULL);
}

void gw_bench_start(void) {
  xTaskCreatePinnedToCore(bench_task, "bench", 8192, NULL, BENCH_PRIORITY, NULL, BENCH_CORE);
}
