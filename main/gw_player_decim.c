/* Hi-res down to 44.1 or 48 kHz (phase 3e). See gw_player_decim.h. The
 * filters are Kaiser-windowed half-bands (beta 11.2), made and checked with
 * scipy on the Mac (2026-11-18): 87 taps for 96 -> 48, 155 for 88.2 -> 44.1,
 * 35 for the first stage of a 4 (192 -> 96, 176.4 -> 88.2). */

#include "gw_player_decim.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"

static const char *TAG = "gw_player_decim";

/* 87 taps, K = 22: h[c +- (2k+1)] = odd[k], h[c] = 0.5 */
static const float hb_96[22] = {
    3.173909783e-01f, -1.033758447e-01f, 5.921057612e-02f, -3.943268955e-02f,
    2.791627683e-02f, -2.028347366e-02f, 1.485799626e-02f, -1.085576043e-02f,
    7.853413932e-03f, -5.593757611e-03f, 3.903811565e-03f, -2.657108940e-03f,
    1.755403704e-03f, -1.119540655e-03f, 6.847995101e-04f, -3.983949136e-04f,
    2.179379953e-04f, -1.102458045e-04f, 5.020679237e-05f, -1.959791189e-05f,
    5.853546099e-06f, -8.392573818e-07f};

/* 155 taps, K = 39: h[c +- (2k+1)] = odd[k], h[c] = 0.5 */
static const float hb_88[39] = {
    3.180230558e-01f, -1.052457690e-01f, 6.224212795e-02f, -4.350448400e-02f,
    3.286986426e-02f, -2.593355812e-02f, 2.100391686e-02f, -1.729327999e-02f,
    1.438583154e-02f, -1.204154454e-02f, 1.011213474e-02f, -8.500874974e-03f,
    7.141711656e-03f, -5.987674929e-03f, 5.004117265e-03f, -4.164589103e-03f,
    3.448247910e-03f, -2.838185057e-03f, 2.320320811e-03f, -1.882669283e-03f,
    1.514840289e-03f, -1.207702677e-03f, 9.531533578e-04f, -7.439596811e-04f,
    5.736494786e-04f, -4.364336783e-04f, 3.271489695e-04f, -2.412130561e-04f,
    1.745867194e-04f, -1.237391989e-04f, 8.561441064e-05f, -5.759665146e-05f,
    3.747522351e-05f, -2.340787796e-05f, 1.388334840e-05f, -7.683542208e-06f,
    3.846007985e-06f, -1.627409461e-06f, 4.686763475e-07f};

/* 35 taps, K = 9: h[c +- (2k+1)] = odd[k], h[c] = 0.5 */
static const float hb_first[9] = {
    3.124739826e-01f, -8.972480148e-02f, 3.970178589e-02f, -1.765857078e-02f,
    7.042333484e-03f, -2.325625625e-03f, 5.722938804e-04f, -8.352546138e-05f,
    2.122832484e-06f};

int decim_factor(int in_rate) {
  switch (in_rate) {
    case 44100: case 48000: return 1;
    case 88200: case 96000: return 2;
    case 176400: case 192000: return 4;
    default: return 0;
  }
}

static void *fast_alloc(size_t n) {
  void *m = heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  return m ? m : heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
}

static bool stage_init(decim_stage_t *s, const float *odd, int K, uint8_t present) {
  memset(s, 0, sizeof(*s));
  s->odd = odd;
  s->K = K;
  s->N = 4 * K - 1;
  s->phase = 0;
  for (int c = 0; c < DECIM_SLOTS; ++c) {
    if (!(present & (1U << c))) continue;
    s->hist[c] = heap_caps_calloc((size_t)s->N - 1U, sizeof(float), MALLOC_CAP_SPIRAM);
    if (!s->hist[c]) return false;
  }
  s->work = fast_alloc(((size_t)s->N - 1U + DECIM_CHUNK) * sizeof(float));
  return s->work != NULL;
}

static void stage_free(decim_stage_t *s) {
  for (int c = 0; c < DECIM_SLOTS; ++c) {
    free(s->hist[c]);
    s->hist[c] = NULL;
  }
  free(s->work);
  s->work = NULL;
}

static void stage_reset(decim_stage_t *s) {
  for (int c = 0; c < DECIM_SLOTS; ++c)
    if (s->hist[c]) memset(s->hist[c], 0, ((size_t)s->N - 1U) * sizeof(float));
  s->phase = 0;
}

/* One channel's n inputs x through the stage: its outputs to y. The phase
 * is the stage's, the same for every channel; the caller moves it on. */
static int stage_block(decim_stage_t *s, int ch, const float *x, int n, float *y) {
  const int N = s->N, c = N / 2, K = s->K;
  float *w = s->work;
  memcpy(w, s->hist[ch], ((size_t)N - 1U) * sizeof(float));
  memcpy(w + N - 1, x, (size_t)n * sizeof(float));
  const float *h = s->odd;
  int m = 0;
  for (int i = s->phase ? 0 : 1; i < n; i += 2) {
    const float *win = w + i; /* the N inputs that end with x[i] */
    const float *lo = win + c - 1, *hi = win + c + 1;
    /* four sums side by side: the FPU's add latency is not waited on */
    float a0 = 0.5f * win[c], a1 = 0.0f, a2 = 0.0f, a3 = 0.0f;
    int k = 0;
    for (; k + 4 <= K; k += 4) {
      a0 += h[k] * (lo[-2 * k] + hi[2 * k]);
      a1 += h[k + 1] * (lo[-2 * k - 2] + hi[2 * k + 2]);
      a2 += h[k + 2] * (lo[-2 * k - 4] + hi[2 * k + 4]);
      a3 += h[k + 3] * (lo[-2 * k - 6] + hi[2 * k + 6]);
    }
    for (; k < K; ++k) a0 += h[k] * (lo[-2 * k] + hi[2 * k]);
    y[m++] = (a0 + a1) + (a2 + a3);
  }
  memcpy(s->hist[ch], w + n, ((size_t)N - 1U) * sizeof(float));
  return m;
}

bool decim_init(decim_t *d, int in_rate, int bits, uint8_t present) {
  memset(d, 0, sizeof(*d));
  d->factor = decim_factor(in_rate);
  d->out_rate = d->factor ? in_rate / d->factor : in_rate;
  d->present = present;
  d->in_scale = bits == 16 ? 256.0f : 1.0f;
  if (d->factor <= 1) return true;
  const bool is48 = in_rate % 48000 == 0;
  const float *last = is48 ? hb_96 : hb_88;
  const int last_k = is48 ? (int)(sizeof(hb_96) / sizeof(hb_96[0]))
                          : (int)(sizeof(hb_88) / sizeof(hb_88[0]));
  bool ok = stage_init(&d->s2, last, last_k, present);
  if (ok && d->factor == 4)
    ok = stage_init(&d->s1, hb_first, (int)(sizeof(hb_first) / sizeof(hb_first[0])), present);
  d->mid = fast_alloc((DECIM_CHUNK / 2 + 1) * sizeof(float));
  d->y = fast_alloc((DECIM_CHUNK / 2 + 1) * sizeof(float));
  ok = ok && d->mid && d->y;
  for (int c = 0; ok && c < DECIM_SLOTS; ++c) {
    if (!(present & (1U << c))) continue;
    d->in[c] = fast_alloc(DECIM_CHUNK * sizeof(float));
    d->out[c] = fast_alloc((DECIM_CHUNK / 2 + 1) * sizeof(int32_t));
    ok = d->in[c] && d->out[c];
  }
  if (!ok) decim_free(d);
  else
    ESP_LOGI(TAG, "/%d: %d + %d taps, work in %s RAM (%u KiB internal free)", d->factor,
             d->factor == 4 ? d->s1.N : 0, d->s2.N,
             esp_ptr_internal(d->s2.work) && esp_ptr_internal(d->y) ? "internal" : "PSRAM",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024U));
  return ok;
}

void decim_reset(decim_t *d) {
  if (d->factor <= 1) return;
  stage_reset(&d->s1);
  stage_reset(&d->s2);
}

void decim_free(decim_t *d) {
  stage_free(&d->s1);
  stage_free(&d->s2);
  free(d->mid);
  free(d->y);
  d->mid = d->y = NULL;
  for (int c = 0; c < DECIM_SLOTS; ++c) {
    free(d->in[c]);
    free(d->out[c]);
    d->in[c] = NULL;
    d->out[c] = NULL;
  }
}

int decim_run(decim_t *d, int n) {
  if (n <= 0) return 0;
  int m = 0;
  const int ph1 = d->s1.phase, ph2 = d->s2.phase;
  for (int c = 0; c < DECIM_SLOTS; ++c) {
    if (!(d->present & (1U << c))) continue;
    const float *x2 = d->in[c];
    int n2 = n;
    if (d->factor == 4) {
      d->s1.phase = ph1;
      n2 = stage_block(&d->s1, c, d->in[c], n, d->mid);
      x2 = d->mid;
    }
    d->s2.phase = ph2;
    m = stage_block(&d->s2, c, x2, n2, d->y);
    int32_t *o = d->out[c];
    for (int j = 0; j < m; ++j) {
      /* round to nearest, then clip: a float compare and trunc.s, no libm */
      float v = d->y[j];
      v = v > 8388607.0f ? 8388607.0f : v < -8388608.0f ? -8388608.0f : v;
      o[j] = (int32_t)(v + (v >= 0.0f ? 0.5f : -0.5f));
    }
    if (d->factor == 4) d->s2.phase = (ph2 + n2) & 1;
  }
  /* the phases move on by what went in */
  if (d->factor == 4) {
    const int n2 = (n + (ph1 ? 1 : 0)) / 2; /* s1's outputs this block */
    d->s1.phase = (ph1 + n) & 1;
    d->s2.phase = (ph2 + n2) & 1;
  } else {
    d->s2.phase = (ph2 + n) & 1;
  }
  return m;
}
