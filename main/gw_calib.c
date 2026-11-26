#include "gw_calib.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "airplay_log.h"
#include "sound_engine.h"
#include "cal_chirp.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gw_mic.h"
#include "gw_local_src.h"
#include "gw_radio.h"
#include "gw_rew.h"
#include "gw_speakers.h"
#include "nvs.h"
#include "rtsp_server.h"

static const char *TAG = "gw_calib";

#define CAL_TASK_STACK      6144U
#define CAL_TASK_PRIORITY   3
#define CAL_NOISE_MS        1000
#define CAL_RECORD_MAX_S    10U
/* A chirp run needs less: the speaker's start-up (~1 s), four chirps (the
 * subwoofer's 2.4 s) and their echoes. Shorter when PSRAM has no room for
 * CAL_RECORD_MAX_S in one block (gw_mic_start). */
#define CAL_CHIRP_RECORD_MIN_S 6U
#define CAL_RESULT_WAIT_MS  15000
#define CAL_SOUND_M_PER_MS  0.343f
/* Ending the sender's session (RTSP stop) takes up to this long. */
#define CAL_SENDER_STOP_MS  8000
/* The chirp is searched from just before it was played to this far after:
 * 60 ms is 20 m of travel; the subwoofer's low-pass adds its own delay. */
#define CAL_SEARCH_WIDE_S   0.060
#define CAL_SEARCH_LOW_S    0.120
#define CAL_SEARCH_EARLY_S  0.002
/* The subwoofer chirp is analysed at 1/8 of the microphone rate. */
#define CAL_LOW_DECIMATE    8U
/* Heard clearly: the recording matches the chirp this well, and the chirps
 * agree on the travel time within this much. */
#define CAL_MIN_MATCH       0.35f
#define CAL_MAX_SPREAD_WIDE_MS 0.5f
#define CAL_MAX_SPREAD_LOW_MS  3.0f
/* The direct sound: the first correlation within -12 dB of the strongest
 * (find_chirp). The chirps' own side lobes of the same sign stay below it
 * (-28 dB, the subwoofer's -15.5 dB). Simulated (a reflection 1-5 ms after the direct sound, 30 rooms
 * of 5 speakers per step): at -6 dB, 145 of 150 distances were off by more
 * than 5 cm with the reflection 6-9 dB louder; at -12 dB none up to 12 dB
 * louder, none at low signal-to-noise, and a 2-way speaker (LR4 crossover,
 * 1.5-3.5 kHz) stays within 4 cm of a full-range one either way. A
 * reflection more than 12 dB louder, or an inverted one as loud as the
 * direct sound, still wins. */
#define CAL_FIRST_ARRIVAL_RATIO 0.25f
/* Level: the RMS from the chirp's arrival over its length and this much of
 * the room after it, as a receiver's noise test measures what the ear
 * hears. The matched filter's peak, used before, also follows the speaker's
 * phase and the room's reflections: the same centre speaker measured -10.5
 * and -4.5 dB in two runs. Wide chirps are high-passed first (room rumble). */
#define CAL_LEVEL_TAIL_WIDE_S 0.010
#define CAL_LEVEL_TAIL_LOW_S  0.050
#define CAL_LEVEL_HP_HZ       200.0
/* A level spread over the chirps beyond this is not trusted. */
#define CAL_MAX_LEVEL_SPREAD_DB 2.0f
/* The quiet room's noise power, in the level's own band, is taken out of
 * each chirp's; the level is used only this far above it. What is left is
 * the noise changing between the quiet room and the chirp: off by half
 * (3 dB), it moves the level by 0.2 dB here, 0.5 dB at 6 dB. Simulated
 * (40 rooms of 5 speakers, noise -62 dBFS): every trim within its 0.5 dB
 * rounding. Without the noise taken out and asking 20 dB, no speaker of
 * those rooms got a level, nor did FL and FR on the boards (17 and 19.6 dB
 * above the noise). */
#define CAL_MIN_LEVEL_SNR_DB 10.0f
/* A recording peak at or above this is clipped (the INMP441's full scale is
 * 120 dB SPL): the level reads low, and louder only clips more. */
#define CAL_CLIP_DBFS (-1.0f)
/* Not heard clearly at the loudest level, the chirp's shape not found
 * (match under CAL_MIN_MATCH) and less than this above the noise: the
 * speaker made no sound (a loose cable, an amplifier off), and its note
 * says so. Simulated, a working speaker in a noisy room (-40 dBFS) still
 * matched 0.39 at under 6 dB, a speaker without its cable 0.12. */
#define CAL_SILENT_SNR_DB 6.0f
/* Levels tried, quietest first, until the travel time and, for a
 * full-range speaker, the level are measured. */
static const int8_t k_levels_db[] = {-30, -24, -18, -12};
/* Without a microphone the chirps are only played once, at this level. */
#define CAL_PLAY_ONLY_DB    (-24)
#define CAL_AAC_SPEAKER_LFE 3 /* aac_speaker_t of the subwoofer */

static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_result_sem;
static gw_cal_status_t s_status;
static _Atomic bool s_running;
/* The CAL_RESULT being waited for (gw_calib_on_result). */
static uint8_t s_wait_mac[6];
static uint8_t s_wait_seq;
static bool s_waiting;
static surround_cal_result_t s_result;
static uint8_t s_seq;

typedef enum { CAL_MODE_TEST, CAL_MODE_FULL, CAL_MODE_SWEEP, CAL_MODE_LEVELS } cal_mode_t;
static struct {
  uint8_t mac[6];
  bool raw, per_way;
  gw_cal_sweep_opts_t opts;
} s_sweep_req;
static uint32_t s_sweep_id;
static int s_sweep_readers; /* downloads of the sweep under way (s_lock) */

static void set_message(gw_cal_state_t state, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
static void set_message(gw_cal_state_t state, const char *fmt, ...) {
  xSemaphoreTake(s_lock, portMAX_DELAY);
  s_status.state = state;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(s_status.message, sizeof(s_status.message), fmt, ap);
  va_end(ap);
  xSemaphoreGive(s_lock);
  AP_LOGI(TAG, "%s", s_status.message);
}

void gw_calib_on_result(const uint8_t mac[6], const surround_cal_result_t *r) {
  if (!s_lock) return;
  /* The speakers task holds its registry lock here. Never call into that
   * registry while holding s_lock in the calibration task. */
  xSemaphoreTake(s_lock, portMAX_DELAY);
  if (s_waiting && r->seq == s_wait_seq &&
      memcmp(mac, s_wait_mac, sizeof(s_wait_mac)) == 0) {
    s_result = *r;
    s_waiting = false;
    xSemaphoreGive(s_result_sem);
  }
  xSemaphoreGive(s_lock);
}

static float to_db(double v) { return v > 1e-12 ? (float)(20.0 * log10(v)) : -240.0f; }

/* The note when the recording could not start. */
static void mic_note(char *note, size_t size, esp_err_t err) {
  if (err == ESP_ERR_NO_MEM) {
    snprintf(note, size, "No memory for the recording (%u KiB free in one block)",
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024U));
  } else {
    snprintf(note, size, "The microphone could not start: %s", esp_err_to_name(err));
  }
}

/* The chirp at the analysis rate. */
static float *make_reference(cal_chirp_kind_t kind, uint32_t rate, uint32_t *len,
                             double *energy) {
  *len = (uint32_t)ceil(cal_chirp_length_s(kind) * rate);
  float *r = heap_caps_malloc(*len * sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!r) return NULL;
  *energy = 0.0;
  for (uint32_t j = 0; j < *len; ++j) {
    r[j] = cal_chirp_value(kind, (double)j / rate);
    *energy += (double)r[j] * r[j];
  }
  return r;
}

typedef struct {
  bool found;
  double index;  /* fractional, at the analysis rate */
  float match;   /* normalised correlation, signed */
} chirp_hit_t;

static double correlate(const float *x, const float *ref, uint32_t len) {
  float acc = 0.0f;
  for (uint32_t j = 0; j < len; ++j) acc += x[j] * ref[j];
  return acc;
}

/* Matched filter over x[0..n): the arrival of the direct sound. The best
 * fit, used before for the time, can be a reflection: normalised by the
 * energy under it, an echo off a wall can fit the chirp's shape better than
 * the direct sound, and one louder than it wins anyway. The direct sound
 * always arrives first. So the time is the strongest correlation, then the
 * first lag of the same sign within CAL_FIRST_ARRIVAL_RATIO of it, and the
 * peak of that run. `match` stays the best fit: how clearly the chirp is
 * heard at all. */
static chirp_hit_t find_chirp(const float *x, uint32_t n, const float *ref,
                              uint32_t len, double ref_energy) {
  chirp_hit_t hit = {0};
  if (n <= len + 2U) return hit;
  const uint32_t lags = n - len;
  float *c = heap_caps_malloc((lags + 1U) * sizeof(float),
                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!c) return hit;
  uint32_t strongest = 0;
  double ex = 0.0; /* energy of x under the reference, sliding */
  for (uint32_t j = 0; j < len; ++j) ex += (double)x[j] * x[j];
  double best_fit = 0.0;
  for (uint32_t lag = 0; lag <= lags; ++lag) {
    if (lag > 0) {
      ex += (double)x[lag + len - 1] * x[lag + len - 1] -
            (double)x[lag - 1] * x[lag - 1];
    }
    c[lag] = (float)correlate(x + lag, ref, len);
    if (fabsf(c[lag]) > fabsf(c[strongest])) strongest = lag;
    const double fit = ex > 0.0 ? c[lag] / sqrt(ex * ref_energy) : 0.0;
    if (fabs(fit) > fabs(best_fit)) best_fit = fit;
  }
  if (c[strongest] == 0.0f) {
    heap_caps_free(c);
    return hit;
  }
  /* The same sign: the chirp's opposite half cycle next to the peak is not
   * an arrival (-10 dB for the wide chirp, -5 dB for the subwoofer's). */
  const float sign = c[strongest] > 0.0f ? 1.0f : -1.0f;
  const float floor_c = fabsf(c[strongest]) * CAL_FIRST_ARRIVAL_RATIO;
  uint32_t lag = 0;
  while (lag < strongest && sign * c[lag] < floor_c) ++lag;
  uint32_t best_lag = lag;
  for (; lag <= lags && sign * c[lag] >= floor_c; ++lag) {
    if (sign * c[lag] > sign * c[best_lag]) best_lag = lag;
  }

  hit.found = true;
  /* Signed like the arrival: a best fit of the other sign (an inverted
   * echo) does not make the speaker inverted. */
  hit.match = (float)(sign * fabs(best_fit));
  /* A parabola through the peak and its neighbours: sub-sample timing. */
  double frac = 0.0;
  if (best_lag > 0U && best_lag < lags) {
    const double a = sign * c[best_lag - 1U];
    const double b = sign * c[best_lag];
    const double d = sign * c[best_lag + 1U];
    const double den = a - 2.0 * b + d;
    if (den < 0.0) frac = 0.5 * (a - d) / den;
    if (frac < -0.5 || frac > 0.5) frac = 0.0;
  }
  hit.index = (double)best_lag + frac;
  heap_caps_free(c);
  return hit;
}

/* A microphone sample (gw_mic.h: 16 bits) at full scale 1.0. */
#define MIC_K (1.0f / GW_MIC_FULL_SCALE)

/* Mean square of x[from, from + len) after a 2nd-order Butterworth high-pass
 * at hp_hz (0: only the mean removed). The filter runs from x[0], so it has
 * settled by `from`. x is floats (xf) or the microphone's samples (xi). */
static double band_ms(const float *xf, const int16_t *xi, uint32_t n, uint32_t from,
                      uint32_t len, uint32_t rate, double hp_hz) {
  if (from >= n) return 0.0;
  if (from + len > n) len = n - from;
  if (len == 0U) return 0.0;
  double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
  if (hp_hz > 0.0) {
    const double w = 2.0 * M_PI * hp_hz / (double)rate;
    const double alpha = sin(w) / (2.0 * M_SQRT1_2);
    const double c = cos(w), a0 = 1.0 + alpha;
    b0 = (1.0 + c) / 2.0 / a0;
    b1 = -(1.0 + c) / a0;
    b2 = b0;
    a1 = -2.0 * c / a0;
    a2 = (1.0 - alpha) / a0;
  }
#define BAND_X(i) (xf ? (double)xf[i] : (double)xi[i] * MIC_K)
  double mean = 0.0;
  for (uint32_t i = 0; i < from + len; ++i) mean += BAND_X(i);
  mean /= (double)(from + len);
  double x1 = 0, x2 = 0, y1 = 0, y2 = 0, sq = 0;
  for (uint32_t i = 0; i < from + len; ++i) {
    const double v = BAND_X(i) - mean;
    const double y = b0 * v + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
    x2 = x1;
    x1 = v;
    y2 = y1;
    y1 = y;
    if (i >= from) sq += y * y;
  }
#undef BAND_X
  return sq / (double)len;
}

static double band_mean_square(const float *x, uint32_t n, uint32_t from, uint32_t len,
                               uint32_t rate, double hp_hz) {
  return band_ms(x, NULL, n, from, len, rate, hp_hz);
}

static double band_mean_square16(const int16_t *x, uint32_t n, uint32_t from, uint32_t len,
                                 uint32_t rate, double hp_hz) {
  return band_ms(NULL, x, n, from, len, rate, hp_hz);
}

/* The quiet room's noise as each kind of chirp's level sees it. */
typedef struct {
  double wide_ms; /* mean square high-passed at CAL_LEVEL_HP_HZ */
  double low_ms;  /* mean square averaged down by CAL_LOW_DECIMATE */
} cal_noise_t;

/* Room noise from a short recording; false when the microphone gives
 * nothing usable. */
static bool measure_noise(float *rms_dbfs, float *peak_dbfs, cal_noise_t *noise,
                          const char **why) {
  if (gw_mic_start(2U, 2U) != ESP_OK) {
    *why = "The microphone could not start";
    return false;
  }
  vTaskDelay(pdMS_TO_TICKS(CAL_NOISE_MS));
  gw_mic_stop();
  uint32_t n = 0;
  const int16_t *x = gw_mic_samples(&n);
  const uint32_t skip = GW_MIC_RATE_HZ / 10U; /* the microphone's start-up */
  if (!x || n <= skip + GW_MIC_RATE_HZ / 4U) {
    *why = "No data from the microphone";
    return false;
  }
  double mean = 0.0;
  for (uint32_t i = skip; i < n; ++i) mean += x[i] * MIC_K;
  mean /= (double)(n - skip);
  double sq = 0.0, peak = 0.0;
  for (uint32_t i = skip; i < n; ++i) {
    const double v = x[i] * MIC_K - mean;
    sq += v * v;
    if (fabs(v) > peak) peak = fabs(v);
  }
  *rms_dbfs = to_db(sqrt(sq / (double)(n - skip)));
  *peak_dbfs = to_db(peak);
  noise->wide_ms = band_mean_square16(x, n, skip, n - skip, GW_MIC_RATE_HZ, CAL_LEVEL_HP_HZ);
  /* The subwoofer's: blocks averaged as its recording is (measure_once). */
  const uint32_t m = (n - skip) / CAL_LOW_DECIMATE;
  double lmean = 0.0, lsq = 0.0;
  for (uint32_t i = 0; i < m; ++i) {
    double acc = 0.0;
    for (uint32_t d = 0; d < CAL_LOW_DECIMATE; ++d) acc += x[skip + i * CAL_LOW_DECIMATE + d] * MIC_K;
    acc /= CAL_LOW_DECIMATE;
    lmean += acc;
    lsq += acc * acc;
  }
  lmean /= (double)m;
  noise->low_ms = lsq / (double)m - lmean * lmean;
  if (*rms_dbfs < -110.0f) {
    *why = "No signal from the microphone: check its wiring";
    return false;
  }
  return true;
}

static int cmp_double(const void *a, const void *b) {
  const double x = *(const double *)a, y = *(const double *)b;
  return (x > y) - (x < y);
}

static double median(double *v, int n) {
  qsort(v, (size_t)n, sizeof(double), cmp_double);
  return n % 2 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

/* Ask one speaker for its chirps and wait until it has played them; the
 * answer is in s_result. False (with a note) when it did not play. */
/* opts: a Room EQ sweep or noise as REW plays it (NULL: the fixed chirp or
 * sweep of the kind); play_ms: how long it plays, beyond the chirps' time. */
static bool request_play(gw_cal_speaker_t *o, cal_chirp_kind_t kind,
                         int8_t level_db, uint8_t bursts, uint8_t flags,
                         const gw_cal_sweep_opts_t *opts, uint32_t play_ms) {
  if (++s_seq == 0U) ++s_seq;
  surround_cal_play_t m = {.seq = s_seq,
                           .kind = (uint8_t)kind,
                           .level_db = level_db,
                           .bursts = bursts,
                           .flags = flags};
  if (opts) {
    m.sweep_ms = opts->sweep_ms;
    m.lo_hz = opts->lo_hz;
    m.hi_hz = opts->hi_hz;
    m.level_rms_db10 = opts->level_db10;
  }
  xSemaphoreTake(s_lock, portMAX_DELAY);
  (void)xSemaphoreTake(s_result_sem, 0);
  memcpy(s_wait_mac, o->mac, sizeof(s_wait_mac));
  s_wait_seq = s_seq;
  s_waiting = true;
  xSemaphoreGive(s_lock);
  const esp_err_t send_err = gw_speakers_cal_play(o->mac, play_ms, &m);
  const bool answered =
      send_err == ESP_OK &&
      xSemaphoreTake(s_result_sem, pdMS_TO_TICKS(CAL_RESULT_WAIT_MS + play_ms)) == pdTRUE;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  s_waiting = false;
  xSemaphoreGive(s_lock);
  if (!answered) {
    snprintf(o->note, sizeof(o->note), "%s",
             send_err == ESP_ERR_NOT_FOUND ? "Not connected" : "No answer from the speaker");
    return false;
  }
  if (s_result.n > SURROUND_CAL_BURSTS_MAX) {
    snprintf(o->note, sizeof(o->note), "Invalid chirp count from the speaker");
    return false;
  }
  if (s_result.err != ESP_OK || s_result.n == 0U) {
    snprintf(o->note, sizeof(o->note), "The speaker could not play: %s",
             esp_err_to_name(s_result.err));
    return false;
  }
  return true;
}

static bool request_chirps(gw_cal_speaker_t *o, cal_chirp_kind_t kind,
                           int8_t level_db) {
  return request_play(o, kind, level_db, CAL_CHIRP_BURSTS, 0U, NULL, 0U);
}

/* Without a microphone: the speaker still plays, so the chirps can be heard,
 * but nothing is measured. */
static void play_only(gw_cal_speaker_t *o, cal_chirp_kind_t kind) {
  if (request_chirps(o, kind, CAL_PLAY_ONLY_DB)) {
    snprintf(o->note, sizeof(o->note), "Played its test chirps; not measured");
  }
}

/* What one run at one level gave, and whether a louder one can help. */
typedef enum {
  RUN_FAILED,   /* no answer, no microphone, or the chirps could not be
                 * placed in the recording: louder changes nothing */
  RUN_CLIPPED,  /* the recording clipped: louder only clips more */
  RUN_UNCLEAR,  /* the travel time is not clear: louder may help */
  RUN_NO_LEVEL, /* the travel time is clear, the level not yet */
  RUN_OK,
} cal_run_t;

/* One run of chirps from one speaker at one level, measured into *o, its
 * note saying what is missing. Every run is logged with its verdict. */
static cal_run_t measure_once(gw_cal_speaker_t *o, cal_chirp_kind_t kind,
                              int8_t level_db, const cal_noise_t *noise) {
  const bool low = kind == CAL_CHIRP_LOW;
  const double noise_ms = low ? noise->low_ms : noise->wide_ms;
  o->low = low;
  const esp_err_t mic_err = gw_mic_start(CAL_CHIRP_RECORD_MIN_S, CAL_RECORD_MAX_S);
  if (mic_err != ESP_OK) {
    mic_note(o->note, sizeof(o->note), mic_err);
    AP_LOGI(TAG, "%s at %d dBFS: %s", o->name, level_db, o->note);
    return RUN_FAILED;
  }
  vTaskDelay(pdMS_TO_TICKS(50));
  const bool played = request_chirps(o, kind, level_db);
  vTaskDelay(pdMS_TO_TICKS(100));
  gw_mic_stop();
  if (!played) {
    AP_LOGI(TAG, "%s at %d dBFS: %s", o->name, level_db, o->note);
    return RUN_FAILED;
  }

  uint32_t n = 0;
  const int16_t *x = gw_mic_samples(&n);
  const uint32_t dec = low ? CAL_LOW_DECIMATE : 1U;
  const uint32_t rate = GW_MIC_RATE_HZ / dec;
  uint32_t len = 0;
  double energy = 0.0;
  float *ref = make_reference(kind, rate, &len, &energy);
  const uint32_t win = (uint32_t)ceil(
      ((low ? CAL_SEARCH_LOW_S : CAL_SEARCH_WIDE_S) + CAL_SEARCH_EARLY_S) * rate) + len;
  float *seg = heap_caps_malloc(win * sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  double tof[SURROUND_CAL_BURSTS_MAX], lev[SURROUND_CAL_BURSTS_MAX],
      match[SURROUND_CAL_BURSTS_MAX];
  const uint32_t level_len =
      len + (uint32_t)((low ? CAL_LEVEL_TAIL_LOW_S : CAL_LEVEL_TAIL_WIDE_S) * rate);
  bool inv[SURROUND_CAL_BURSTS_MAX];
  int hits = 0;
  float peak = 0.0f; /* the raw recording around the chirps */
  for (int k = 0; ref && seg && x && k < s_result.n; ++k) {
    int64_t emit_local = 0;
    double emit_idx = 0.0;
    if (!gw_speakers_tsf_to_local(s_result.emit_tsf_ns[k], &emit_local) ||
        !gw_mic_sample_at(emit_local, &emit_idx))
      continue;
    const double start = emit_idx - CAL_SEARCH_EARLY_S * GW_MIC_RATE_HZ;
    if (start < 0.0) continue;
    const uint32_t s0 = (uint32_t)start;
    if ((uint64_t)s0 + (uint64_t)win * dec >= n) continue;
    for (uint32_t i = 0; i < win * dec; ++i) {
      if (fabsf(x[s0 + i] * MIC_K) > peak) peak = fabsf(x[s0 + i] * MIC_K);
    }
    /* The analysis segment, the subwoofer's averaged down to its rate. */
    for (uint32_t i = 0; i < win; ++i) {
      float acc = 0.0f;
      for (uint32_t d = 0; d < dec; ++d) acc += x[s0 + i * dec + d] * MIC_K;
      seg[i] = acc / (float)dec;
    }
    const chirp_hit_t hit = find_chirp(seg, win, ref, len, energy);
    if (!hit.found) continue;
    int64_t arrival_local = 0;
    if (!gw_mic_sample_time_ns((double)s0 + hit.index * dec, &arrival_local)) continue;
    tof[hits] = (double)(arrival_local - emit_local) / 1e6;
    /* The chirp's own power: the room's noise in the same band taken out. */
    lev[hits] = band_mean_square(seg, win, (uint32_t)hit.index, level_len, rate,
                                 low ? 0.0 : CAL_LEVEL_HP_HZ) - noise_ms;
    match[hits] = fabs(hit.match);
    inv[hits] = hit.match < 0.0f;
    hits++;
  }
  heap_caps_free(seg);
  heap_caps_free(ref);
  /* One hit has zero spread but cannot demonstrate repeatability. Require
   * a majority of the chirps requested, as in the wired latency test.
   * find_chirp finds a peak in any segment, heard or not: a chirp is
   * missing only when its time cannot be placed in the recording (the TSF
   * or the microphone's time line) or memory ran out, never because it was
   * too quiet. */
  if (hits <= CAL_CHIRP_BURSTS / 2) {
    snprintf(o->note, sizeof(o->note), "Only %d of %d chirps placed in the recording",
             hits, CAL_CHIRP_BURSTS);
    AP_LOGI(TAG, "%s at %d dBFS: %s", o->name, level_db, o->note);
    return RUN_FAILED;
  }
  /* A chirp far off the others' travel time (a door, a cough on it) is
   * left out; a majority must agree within the allowed spread. */
  const float max_spread = low ? CAL_MAX_SPREAD_LOW_MS : CAL_MAX_SPREAD_WIDE_MS;
  double sorted[SURROUND_CAL_BURSTS_MAX];
  memcpy(sorted, match, sizeof(match[0]) * (size_t)hits);
  const double match_all = median(sorted, hits);
  memcpy(sorted, tof, sizeof(tof[0]) * (size_t)hits);
  const double tof_mid = median(sorted, hits);
  double all_min = tof[0], all_max = tof[0];
  int used = 0, inverted = 0;
  for (int i = 0; i < hits; ++i) {
    if (tof[i] < all_min) all_min = tof[i];
    if (tof[i] > all_max) all_max = tof[i];
    if (fabs(tof[i] - tof_mid) > max_spread / 2.0) continue;
    tof[used] = tof[i];
    lev[used] = 10.0 * log10(lev[i] > 1e-24 ? lev[i] : 1e-24);
    match[used] = match[i];
    inverted += inv[i];
    used++;
  }
  const bool agree = used > CAL_CHIRP_BURSTS / 2;
  if (!agree) used = 0;
  double tmin = 0, tmax = 0, lmin = 0, lmax = 0;
  for (int i = 0; i < used; ++i) {
    if (i == 0 || tof[i] < tmin) tmin = tof[i];
    if (i == 0 || tof[i] > tmax) tmax = tof[i];
    if (i == 0 || lev[i] < lmin) lmin = lev[i];
    if (i == 0 || lev[i] > lmax) lmax = lev[i];
  }
  const double lev_db = agree ? median(lev, used) : -240.0;
  o->tof_ms = (float)(agree ? median(tof, used) : tof_mid);
  o->distance_m = o->tof_ms * CAL_SOUND_M_PER_MS;
  o->level_db = (float)lev_db - (float)level_db;
  o->level_spread_db = (float)(lmax - lmin);
  o->snr_db = (float)(lev_db - 10.0 * log10(noise_ms + 1e-24));
  o->match = (float)(agree ? median(match, used) : match_all);
  o->spread_ms = (float)(agree ? tmax - tmin : all_max - all_min);
  o->inverted = !low && inverted * 2 > used;
  const float peak_dbfs = to_db(peak);
  const bool clipped = peak_dbfs >= CAL_CLIP_DBFS;
  o->measured = !clipped && agree && o->match >= CAL_MIN_MATCH &&
                o->tof_ms > -1.0f;
  o->level_ok = o->measured && !low && o->snr_db >= CAL_MIN_LEVEL_SNR_DB &&
                o->level_spread_db <= CAL_MAX_LEVEL_SPREAD_DB;
  cal_run_t run;
  if (clipped) {
    run = RUN_CLIPPED;
    snprintf(o->note, sizeof(o->note),
             "Clipped at %d dBFS: move the microphone away from it", level_db);
  } else if (!o->measured) {
    run = RUN_UNCLEAR;
    snprintf(o->note, sizeof(o->note), "Not heard clearly (match %.2f, spread %.2f ms)",
             (double)o->match, (double)o->spread_ms);
  } else if (low || o->level_ok) {
    run = RUN_OK;
    o->note[0] = '\0';
  } else if (o->snr_db < CAL_MIN_LEVEL_SNR_DB) {
    run = RUN_NO_LEVEL;
    snprintf(o->note, sizeof(o->note),
             "Too quiet for a level: %.1f dB above the noise, %.0f needed",
             (double)o->snr_db, (double)CAL_MIN_LEVEL_SNR_DB);
  } else {
    run = RUN_NO_LEVEL;
    snprintf(o->note, sizeof(o->note), "Level not repeatable (%.1f dB over the chirps)",
             (double)o->level_spread_db);
  }
  AP_LOGI(TAG,
          "%s at %d dBFS: %d/%d chirps agree, travel %.2f ms (%.2f m) spread %.2f ms, "
          "level %.1f dB spread %.1f dB, %.1f dB above noise, peak %.1f dBFS, "
          "match %.2f%s: %s",
          o->name, level_db, used, s_result.n, (double)o->tof_ms,
          (double)o->distance_m, (double)o->spread_ms, (double)o->level_db,
          (double)o->level_spread_db, (double)o->snr_db, (double)peak_dbfs,
          (double)o->match, o->inverted ? ", inverted" : "",
          run == RUN_OK ? "measured" : o->note);
  return run;
}

/* Delays: the farthest speaker 0 ms, the others as much later as they are
 * closer. Levels: the quietest 0 dB, the others turned down to it. Both
 * compare speakers: delays need two speakers measured, one of them
 * full-range (the reference), levels two full-range speakers. The
 * subwoofer's chirp is low and its travel time less exact, so it gets a
 * delay but no level, and a note to check it by ear. */
static void suggest(void) {
  float tof_max = -1e9f, level_min = 1e9f;
  int times = 0, levels = 0;
  for (int i = 0; i < s_status.count; ++i) {
    const gw_cal_speaker_t *o = &s_status.spk[i];
    if (!o->measured) continue;
    times++;
    if (o->low) continue;
    if (o->tof_ms > tof_max) tof_max = o->tof_ms;
    if (o->level_ok) {
      levels++;
      if (o->level_db < level_min) level_min = o->level_db;
    }
  }
  if (times < 2 || tof_max < -1e8f) return;
  for (int i = 0; i < s_status.count; ++i) {
    gw_cal_speaker_t *o = &s_status.spk[i];
    if (!o->measured) continue;
    float d = tof_max - o->tof_ms;
    if (d < 0.0f) d = 0.0f;
    if (d > SURROUND_DELAY_MAX_US / 1000.0f) d = SURROUND_DELAY_MAX_US / 1000.0f;
    o->delay_ms = roundf(d * 10.0f) / 10.0f;
    o->has_suggestion = true;
    if (o->low) {
      snprintf(o->note, sizeof(o->note), "Subwoofer: level and delay by ear");
    } else if (o->level_ok && levels < 2) {
      snprintf(o->note, sizeof(o->note), "No other level to compare with: level unchanged");
    } else if (o->level_ok) {
      float t = level_min - o->level_db;
      if (t < SURROUND_TRIM_MIN_DB10 / 10.0f) t = SURROUND_TRIM_MIN_DB10 / 10.0f;
      o->trim_db = roundf(t * 2.0f) / 2.0f;
      o->has_trim = true;
    }
  }
}

/* A sender keeps its session while it is paused, and the speakers can only
 * play chirps between streams: end the session. The RTSP listener stays
 * closed until the calibration is over, so the sender cannot come back in
 * the middle of it; afterwards it is picked again as the audio output. */
static bool stop_sender(void) {
  set_message(GW_CAL_BUSY, "Disconnecting the AirPlay sender");
  rtsp_server_stop();
  for (int i = 0; i < CAL_SENDER_STOP_MS / 100; ++i) {
    if (rtsp_server_is_idle() && sound_engine_is_idle()) return true;
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  return rtsp_server_is_idle() && sound_engine_is_idle();
}

static void store_noise(bool valid, float rms, float peak) {
  xSemaphoreTake(s_lock, portMAX_DELAY);
  s_status.noise_valid = valid;
  s_status.noise_dbfs = rms;
  s_status.peak_dbfs = peak;
  xSemaphoreGive(s_lock);
}

/* Every speaker in turn: measured with the microphone, or only played. */
/* The calibration task's copy of the speaker list. */
static gw_speaker_info_t s_list[SURROUND_MAX_SPEAKERS];

static void run_speakers(bool mic_ok, const char *mic_why, const cal_noise_t *noise) {
  gw_speaker_info_t *list = s_list;
  const size_t n = gw_speakers_list(s_list);
  gw_cal_speaker_t spk[SURROUND_MAX_SPEAKERS];
  uint8_t count = 0;
  for (size_t i = 0; i < n; ++i) {
    if (!list[i].config_valid) continue;
    gw_cal_speaker_t *o = &spk[count++];
    memset(o, 0, sizeof(*o));
    memcpy(o->mac, list[i].mac, sizeof(o->mac));
    strlcpy(o->name, list[i].name[0] ? list[i].name : list[i].ip, sizeof(o->name));
    o->channel = list[i].config.channel;
  }
  xSemaphoreTake(s_lock, portMAX_DELAY);
  s_status.count = count;
  s_status.steps = count;
  memcpy(s_status.spk, spk, sizeof(spk[0]) * count);
  xSemaphoreGive(s_lock);
  if (count == 0) {
    set_message(GW_CAL_FAILED, "No speaker connected");
    return;
  }
  for (uint8_t i = 0; i < count; ++i) {
    gw_cal_speaker_t o = spk[i];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.step = (uint8_t)(i + 1U);
    xSemaphoreGive(s_lock);
    set_message(GW_CAL_BUSY, "%s %u of %u: %s", mic_ok ? "Measuring" : "Playing",
                (unsigned)(i + 1U), (unsigned)count, o.name);
    const cal_chirp_kind_t kind =
        o.channel == CAL_AAC_SPEAKER_LFE ? CAL_CHIRP_LOW : CAL_CHIRP_WIDE;
    if (!mic_ok) {
      play_only(&o, kind);
    } else {
      /* Louder while it can help. A run with a clear travel time is kept
       * when a louder one does worse; a clipped one says why it stopped. */
      gw_cal_speaker_t kept = o;
      bool have_time = false;
      for (size_t l = 0; l < sizeof(k_levels_db); ++l) {
        gw_cal_speaker_t t = o;
        const cal_run_t run = measure_once(&t, kind, k_levels_db[l], noise);
        if (run == RUN_UNCLEAR && !have_time && l + 1U == sizeof(k_levels_db) &&
            t.match < CAL_MIN_MATCH && t.snr_db < CAL_SILENT_SNR_DB) {
          t.silent = true;
          snprintf(t.note, sizeof(t.note),
                   "No sound at %d dBFS: check its cable and amplifier", k_levels_db[l]);
          AP_LOGI(TAG, "%s: no sound, left unchanged", t.name);
        }
        if (run == RUN_OK || run == RUN_NO_LEVEL) {
          kept = t;
          have_time = true;
        } else if (!have_time) {
          kept = t;
        } else if (run == RUN_CLIPPED) {
          memcpy(kept.note, t.note, sizeof(kept.note));
        }
        if (run == RUN_OK || run == RUN_FAILED || run == RUN_CLIPPED) break;
        if (!sound_engine_is_idle()) break; /* a stream started */
      }
      o = kept;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.spk[i] = o;
    xSemaphoreGive(s_lock);
  }
  if (!mic_ok) {
    set_message(GW_CAL_FAILED,
                "%s. The speakers played their test chirps, but nothing was measured.",
                mic_why ? mic_why : "No microphone");
    return;
  }
  /* What was measured is applied; the rest stays as it is, named here and
   * in each speaker's note. */
  char no_delay[64] = "", no_level[64] = "";
  int delays = 0;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  suggest();
  for (int i = 0; i < s_status.count; ++i) {
    const gw_cal_speaker_t *o = &s_status.spk[i];
    char *list = NULL;
    if (!o->has_suggestion) list = no_delay;
    else if (!o->low && !o->has_trim) list = no_level;
    if (o->has_suggestion) delays++;
    if (!list) continue;
    if (list[0]) strlcat(list, ", ", sizeof(no_delay));
    strlcat(list, o->name, sizeof(no_delay));
  }
  xSemaphoreGive(s_lock);
  if (delays == 0) {
    set_message(GW_CAL_FAILED,
                "Nothing to apply: fewer than two speakers measured (see the notes)");
  } else if (!no_delay[0] && !no_level[0]) {
    set_message(GW_CAL_DONE,
                "Done: all %u speakers measured. Nothing changes until you apply.",
                (unsigned)count);
  } else {
    set_message(GW_CAL_DONE,
                "Done. Unchanged: %s%s%s%s%s. Nothing changes until you apply.",
                no_delay, no_delay[0] ? " (not measured)" : "",
                no_delay[0] && no_level[0] ? "; " : "",
                no_level[0] ? "level of " : "", no_level);
  }
}

/* ---- Room EQ: one speaker's sweeps ---- */
#define CAL_SWEEP_MIN_SNR_DB 20.0f  /* broadband, before the sweep's own gain */
#define CAL_SWEEP_PRE_S      0.1    /* served before the first sweep */

typedef enum { SWEEP_OK, SWEEP_QUIET, SWEEP_CLIPPED, SWEEP_FAILED } sweep_run_t;

/* How the speaker plays opts: the sweep's definition and the time the
 * whole run takes (all its sweeps), as the speaker computes them. */
static bool sweep_plan(const gw_cal_sweep_opts_t *opts, int sweeps, cal_sweep_def_t *def,
                       uint32_t *play_ms) {
  if (!cal_sweep_custom(opts->sweep_ms, opts->lo_hz, opts->hi_hz, def)) return false;
  *play_ms = (uint32_t)ceil(((double)sweeps * def->period_s + 1.0) * 1000.0);
  return true;
}

/* The sweeps of one run into *sw at the level asked, as REW: no level steps.
 * The recording stays in gw_mic. */
static sweep_run_t sweep_once(gw_cal_speaker_t *o, cal_chirp_kind_t kind,
                              const gw_cal_sweep_opts_t *opts, int sweeps,
                              uint8_t flags, double noise_ms, gw_cal_sweep_t *sw) {
  const bool low = cal_chirp_is_low(kind);
  cal_sweep_def_t def;
  uint32_t play_ms = 0;
  if (!sweep_plan(opts, sweeps, &def, &play_ms)) {
    snprintf(o->note, sizeof(o->note), "That sweep is not possible");
    return SWEEP_FAILED;
  }
  const uint32_t rec_s = (play_ms + 1999U) / 1000U;
  const esp_err_t mic_err = gw_mic_start(rec_s, rec_s);
  if (mic_err != ESP_OK) {
    mic_note(o->note, sizeof(o->note), mic_err);
    return SWEEP_FAILED;
  }
  vTaskDelay(pdMS_TO_TICKS(50));
  const bool played = request_play(o, kind, 0, opts->repeats, flags, opts, play_ms);
  vTaskDelay(pdMS_TO_TICKS(100));
  gw_mic_stop();
  if (!played) return SWEEP_FAILED;

  uint32_t n = 0;
  const int16_t *x = gw_mic_samples(&n);
  const uint32_t rate = GW_MIC_RATE_HZ;
  const uint32_t len = (uint32_t)ceil(def.length_s * rate);
  const uint32_t period = (uint32_t)ceil(def.period_s * rate);
  const uint32_t pre = (uint32_t)(CAL_SWEEP_PRE_S * rate);
  memset(sw, 0, sizeof(*sw));
  sw->n = s_result.n;
  double first = 0.0, last = 0.0;
  for (int k = 0; x && k < s_result.n; ++k) {
    int64_t emit_local = 0;
    double idx = 0.0;
    if (!gw_speakers_tsf_to_local(s_result.emit_tsf_ns[k], &emit_local) ||
        !gw_mic_sample_at(emit_local, &idx) || idx < (double)pre ||
        idx + (double)period > (double)n) {
      snprintf(o->note, sizeof(o->note), "Sweep %d of %d not placed in the recording",
               k + 1, s_result.n);
      AP_LOGI(TAG, "%s: %s", o->name, o->note);
      return SWEEP_FAILED;
    }
    if (k == 0) first = idx;
    last = idx;
    sw->start[k] = idx;
    sw->way[k] = s_result.way[k];
    const uint32_t from = (uint32_t)idx - pre;
    const double ms = band_mean_square16(x + from, n - from, pre, len, rate,
                                         low ? 0.0 : CAL_LEVEL_HP_HZ) - noise_ms;
    sw->snr_db[k] = (float)(10.0 * log10((ms > 1e-24 ? ms : 1e-24) / (noise_ms + 1e-24)));
  }
  if (!x || s_result.n == 0U) return SWEEP_FAILED;
  sw->from = (uint32_t)(first - (double)pre);
  const uint32_t to = (uint32_t)ceil(last) + period;
  sw->count = (to < n ? to : n) - sw->from;
  float peak = 0.0f;
  for (uint32_t i = sw->from; i < sw->from + sw->count; ++i) {
    if (fabsf(x[i] * MIC_K) > peak) peak = fabsf(x[i] * MIC_K);
  }
  for (int k = 0; k < sw->n; ++k) sw->start[k] -= (double)sw->from;
  sw->peak_dbfs = to_db(peak);
  sw->scale = peak > 1e-7f ? 0.98f / peak : 1.0f;
  sw->level_rms_db = (float)opts->level_db10 / 10.0f;
  sw->level_db = sw->level_rms_db + 3.01f; /* a sine sweep's peak */
  sw->def = def;
  sw->lo_hz = opts->lo_hz;
  sw->hi_hz = opts->hi_hz;
  sw->repeats = opts->repeats;
  float snr = sw->snr_db[0];
  for (int k = 1; k < sw->n; ++k) {
    if (sw->snr_db[k] < snr) snr = sw->snr_db[k];
  }
  sweep_run_t run = SWEEP_OK;
  if (sw->peak_dbfs >= CAL_CLIP_DBFS) {
    run = SWEEP_CLIPPED;
  } else if (snr < CAL_SWEEP_MIN_SNR_DB) {
    run = SWEEP_QUIET;
  }
  AP_LOGI(TAG, "%s %s %.1f s %u-%u Hz at %.1f dBFS RMS: %d sweeps (way 0x%02x 0x%02x), "
               "%.1f dB above noise, peak %.1f dBFS: %s",
          o->name, cal_chirp_name(kind), def.length_s, (unsigned)opts->lo_hz,
          (unsigned)opts->hi_hz, (double)sw->level_rms_db, sw->n, (unsigned)sw->way[0],
          (unsigned)(sw->n > 1 ? sw->way[1] : 0U), (double)snr, (double)sw->peak_dbfs,
          run == SWEEP_OK ? "measured" : run == SWEEP_CLIPPED ? "clipped" : "quiet");
  return run;
}

static void way_names(const gw_cal_sweep_t *sw, char *out, size_t size) {
  out[0] = '\0';
  for (int k = 0; k < sw->n; ++k) {
    const uint8_t w = sw->way[k];
    if (k) strlcat(out, ", ", size);
    strlcat(out, (w & SURROUND_CAL_WAY_TWEETER) && !(w & SURROUND_CAL_WAY_WOOFER) ? "tweeter"
                 : (w & SURROUND_CAL_WAY_WOOFER) && !(w & SURROUND_CAL_WAY_TWEETER) ? "woofer"
                                                                                  : "both outputs",
            size);
  }
}

/* The speaker the request names, from the list: false if not connected. */
static bool find_request_speaker(gw_cal_speaker_t *o) {
  gw_speaker_info_t *list = s_list;
  const size_t count = gw_speakers_list(s_list);
  memset(o, 0, sizeof(*o));
  for (size_t i = 0; i < count; ++i) {
    if (!list[i].config_valid || memcmp(list[i].mac, s_sweep_req.mac, 6) != 0) continue;
    memcpy(o->mac, list[i].mac, sizeof(o->mac));
    strlcpy(o->name, list[i].name[0] ? list[i].name : list[i].ip, sizeof(o->name));
    o->channel = list[i].config.channel;
    return true;
  }
  return false;
}

static void run_sweep(bool mic_ok, const char *mic_why, const cal_noise_t *noise,
                      float noise_dbfs) {
  if (!mic_ok) {
    set_message(GW_CAL_FAILED, "%s", mic_why ? mic_why : "No microphone");
    return;
  }
  gw_cal_speaker_t o;
  if (!find_request_speaker(&o)) {
    set_message(GW_CAL_FAILED, "The speaker is not connected");
    return;
  }
  /* The subwoofer's sweep is low and never per driver; the kind still
   * tells the speaker which (its fixed sweeps stay for older pages). */
  const cal_chirp_kind_t kind =
      o.channel == CAL_AAC_SPEAKER_LFE ? CAL_SWEEP_LOW : CAL_SWEEP_WIDE;
  const bool per_way = s_sweep_req.per_way && kind == CAL_SWEEP_WIDE;
  const gw_cal_sweep_opts_t *opts = &s_sweep_req.opts;
  const bool all = per_way && opts->all_ways;
  const uint8_t flags = (s_sweep_req.raw ? SURROUND_CAL_RAW : 0U) |
                        (per_way ? SURROUND_CAL_PER_WAY : 0U) | (all ? SURROUND_CAL_ALL_WAYS : 0U);
  const double noise_ms = cal_chirp_is_low(kind)
                              ? pow(10.0, (double)noise_dbfs / 10.0)
                              : noise->wide_ms;
  /* Per driver: each driver plays every repeat (the speaker alternates). */
  const int sweeps = (all ? 3 : per_way ? 2 : 1) * (int)opts->repeats;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  s_status.steps = 1;
  s_status.step = 1;
  xSemaphoreGive(s_lock);
  set_message(GW_CAL_BUSY, "Measuring %s: %d sweep%s of %.1f s at %.1f dBFS RMS", o.name,
              sweeps, sweeps > 1 ? "s" : "", opts->sweep_ms / 1000.0, opts->level_db10 / 10.0);
  gw_cal_sweep_t sw = {0};
  const sweep_run_t run = sweep_once(&o, kind, opts, sweeps, flags, noise_ms, &sw);
  if (run == SWEEP_CLIPPED) {
    set_message(GW_CAL_FAILED, "Clipped (%.1f dBFS at the microphone): turn the level down "
                "or move the microphone away from %s", (double)sw.peak_dbfs, o.name);
    return;
  }
  if (run == SWEEP_FAILED) {
    set_message(GW_CAL_FAILED, "%s: %s", o.name, o.note[0] ? o.note : "not measured");
    return;
  }
  memcpy(sw.mac, o.mac, sizeof(sw.mac));
  strlcpy(sw.name, o.name, sizeof(sw.name));
  sw.channel = o.channel;
  sw.kind = (uint8_t)kind;
  sw.raw = s_sweep_req.raw;
  sw.id = ++s_sweep_id;
  sw.valid = true;
  float snr = sw.snr_db[0];
  for (int k = 1; k < sw.n; ++k) {
    if (sw.snr_db[k] < snr) snr = sw.snr_db[k];
  }
  char ways[48];
  way_names(&sw, ways, sizeof(ways));
  xSemaphoreTake(s_lock, portMAX_DELAY);
  s_status.sweep = sw;
  xSemaphoreGive(s_lock);
  set_message(GW_CAL_DONE, "Measured %s (%s) at %.1f dBFS RMS, %.0f dB above the room noise.%s",
              o.name, ways, (double)sw.level_rms_db, (double)snr,
              run == SWEEP_QUIET ? " Quiet: a longer sweep, more repeats or a higher level "
                                   "(up to the speaker's limit) help." : "");
}

/* Check levels, as REW's: pink noise for CAL_NOISE_S at the level of the
 * next measurement; the microphone's RMS and peak over its middle. */
static void run_levels(bool mic_ok, const char *mic_why, float noise_dbfs) {
  if (!mic_ok) {
    set_message(GW_CAL_FAILED, "%s", mic_why ? mic_why : "No microphone");
    return;
  }
  gw_cal_speaker_t o;
  if (!find_request_speaker(&o)) {
    set_message(GW_CAL_FAILED, "The speaker is not connected");
    return;
  }
  /* The noise never above GW_CAL_NOISE_MAX_DB10, where its 4x peaks reach
   * full scale: louder, the speaker would clip it. The result is scaled up
   * by the difference. */
  gw_cal_sweep_opts_t play_opts = s_sweep_req.opts;
  const int16_t asked_db10 = play_opts.level_db10;
  if (play_opts.level_db10 > GW_CAL_NOISE_MAX_DB10) play_opts.level_db10 = GW_CAL_NOISE_MAX_DB10;
  const gw_cal_sweep_opts_t *opts = &play_opts;
  const float delta_db = (float)(asked_db10 - play_opts.level_db10) / 10.0f;
  set_message(GW_CAL_BUSY, "Check levels: pink noise from %s at %.1f dBFS RMS%s", o.name,
              opts->level_db10 / 10.0, delta_db > 0.0f ? " (scaled up to the level asked)" : "");
  const uint32_t play_ms = (uint32_t)(CAL_NOISE_S * 1000.0) + 1000U;
  const uint32_t rec_s = (play_ms + 1999U) / 1000U;
  const esp_err_t mic_err = gw_mic_start(rec_s, rec_s);
  if (mic_err != ESP_OK) {
    mic_note(o.note, sizeof(o.note), mic_err);
    set_message(GW_CAL_FAILED, "%s", o.note);
    return;
  }
  vTaskDelay(pdMS_TO_TICKS(50));
  const bool played = request_play(&o, CAL_NOISE_PINK, 0, 1U, SURROUND_CAL_RAW, opts, play_ms);
  vTaskDelay(pdMS_TO_TICKS(100));
  gw_mic_stop();
  uint32_t n = 0;
  const int16_t *x = gw_mic_samples(&n);
  int64_t emit_local = 0;
  double idx = 0.0;
  const uint32_t rate = GW_MIC_RATE_HZ;
  if (!played || !x || s_result.n == 0U ||
      !gw_speakers_tsf_to_local(s_result.emit_tsf_ns[0], &emit_local) ||
      !gw_mic_sample_at(emit_local, &idx) || idx < 0.0 ||
      idx + CAL_NOISE_S * rate > (double)n) {
    gw_mic_release();
    set_message(GW_CAL_FAILED, "%s: %s", o.name, o.note[0] ? o.note : "the noise was not heard");
    return;
  }
  /* The middle of it: past the room's build-up, before the end. */
  const uint32_t from = (uint32_t)idx + rate / 2U, to = (uint32_t)idx + (uint32_t)(CAL_NOISE_S * rate) - rate / 4U;
  double mean = 0.0;
  for (uint32_t i = from; i < to; ++i) mean += x[i] * MIC_K;
  mean /= (double)(to - from);
  double sq = 0.0, peak = 0.0;
  for (uint32_t i = from; i < to; ++i) {
    const double v = x[i] * MIC_K - mean;
    sq += v * v;
    if (fabs(v) > peak) peak = fabs(v);
  }
  gw_mic_release();
  const float real_peak_dbfs = to_db(peak);
  gw_cal_levels_t lv = {.valid = true,
                        .level_rms_db = (float)asked_db10 / 10.0f,
                        .rms_dbfs = to_db(sqrt(sq / (double)(to - from))) + delta_db,
                        .peak_dbfs = real_peak_dbfs + delta_db,
                        .noise_dbfs = noise_dbfs,
                        .estimated = delta_db > 0.0f,
                        .played_rms_db = (float)opts->level_db10 / 10.0f};
  /* Clipping is what the microphone really recorded, not the estimate. */
  lv.clipped = real_peak_dbfs >= CAL_CLIP_DBFS;
  memcpy(lv.mac, o.mac, sizeof(lv.mac));
  strlcpy(lv.name, o.name, sizeof(lv.name));
  xSemaphoreTake(s_lock, portMAX_DELAY);
  s_status.levels = lv;
  xSemaphoreGive(s_lock);
  AP_LOGI(TAG, "Check levels %s, asked %.1f, played %.1f dBFS RMS: microphone %.1f dBFS RMS, "
               "peak %.1f%s, room noise %.1f dBFS%s",
          o.name, (double)lv.level_rms_db, (double)lv.played_rms_db, (double)lv.rms_dbfs,
          (double)lv.peak_dbfs, lv.estimated ? " (scaled)" : "", (double)noise_dbfs,
          lv.clipped ? ", clipped" : "");
  set_message(lv.clipped ? GW_CAL_FAILED : GW_CAL_DONE,
              "%s: %.1f dBFS RMS at the microphone, %.0f dB above the room noise%s", o.name,
              (double)lv.rms_dbfs, (double)(lv.rms_dbfs - noise_dbfs),
              lv.clipped ? "; clipped: turn the level down" : "");
}

static void calib_task(void *arg) {
  const cal_mode_t mode = (cal_mode_t)(uintptr_t)arg;
  const bool mic_test = mode == CAL_MODE_TEST;
  bool stopped_airplay = false;
  float rms = 0.0f, peak = 0.0f;
  cal_noise_t noise = {0};
  const char *why = NULL;
  /* 1. Is there a microphone at all? This works while music plays, so the
   *    result is known before the sender is disconnected. */
  set_message(GW_CAL_BUSY, "Checking the microphone");
  bool mic_ok = measure_noise(&rms, &peak, &noise, &why);
  if (mic_test) {
    store_noise(mic_ok, rms, peak);
    if (mic_ok) {
      set_message(GW_CAL_DONE, "Microphone works; room noise %.0f dBFS", (double)rms);
    } else {
      set_message(GW_CAL_FAILED, "%s", why);
    }
  } else {
    /* Reserve the listener even without an active stream: a new sender must
     * not enter during the quiet-room recording or the chirps. */
    const bool had_sender = !sound_engine_is_idle();
    const bool quiet = stop_sender();
    stopped_airplay = had_sender;
    if (!quiet) {
      set_message(GW_CAL_FAILED, "The AirPlay sender did not let go; try again");
    } else {
      /* 3. The noise of the quiet room, then the speakers. Without a
       *    microphone they still play, so the procedure can be heard. */
      if (mic_ok) {
        set_message(GW_CAL_BUSY, "Listening to the room");
        mic_ok = measure_noise(&rms, &peak, &noise, &why);
      }
      store_noise(mic_ok, rms, peak);
      if (mode == CAL_MODE_SWEEP) {
        run_sweep(mic_ok, why, &noise, rms);
      } else if (mode == CAL_MODE_LEVELS) {
        run_levels(mic_ok, why, rms);
      } else {
        run_speakers(mic_ok, why, &noise);
      }
    }
  }
  /* The recording is no longer needed, except a sweep's: the Room EQ page
   * fetches it (gw_calib_sweep_samples), and it goes when the USB player or
   * the radio starts (gw_calib_release_recording). Kept, ~2 MiB of PSRAM
   * would be missing for them. */
  if (mode != CAL_MODE_SWEEP) gw_mic_release();
  if (!mic_test) {
    /* AirPlay again; also after a stop that timed out. */
    if (rtsp_server_is_idle()) {
      const esp_err_t err = rtsp_server_start();
      if (err != ESP_OK) {
        ESP_LOGE(TAG, "AirPlay restart failed: %s", esp_err_to_name(err));
        set_message(GW_CAL_FAILED, "AirPlay could not restart: %s", esp_err_to_name(err));
      }
    }
  }
  xSemaphoreTake(s_lock, portMAX_DELAY);
  s_status.sender_stopped = stopped_airplay;
  s_running = false;
  xSemaphoreGive(s_lock);
  vTaskDelete(NULL);
}

static bool init_once(void) {
  /* Initialised by the HTTP task; retry either allocation after low memory. */
  if (!s_lock) s_lock = xSemaphoreCreateMutex();
  if (!s_result_sem) s_result_sem = xSemaphoreCreateBinary();
  return s_lock && s_result_sem;
}

static esp_err_t start_mode(cal_mode_t mode) {
#if !(defined(CONFIG_AIRPLAY_SURROUND_CAL_MIC) && CONFIG_AIRPLAY_SURROUND_CAL_MIC)
  (void)mode;
  return ESP_ERR_NOT_SUPPORTED;
#else
  const bool mic_test_only = mode == CAL_MODE_TEST;
  if (gw_rew_active()) return ESP_ERR_INVALID_STATE; /* the REW mode has the speakers */
  if (!init_once()) return ESP_ERR_NO_MEM;
  gw_local_src_stop(); /* the chirps need the speakers free */
  xSemaphoreTake(s_lock, portMAX_DELAY);
  if (s_running) {
    xSemaphoreGive(s_lock);
    return ESP_ERR_INVALID_STATE;
  }
  s_running = true;
  memset(&s_status, 0, sizeof(s_status));
  s_status.mic_built = true;
  s_status.mic_test = mic_test_only;
  s_status.sweep_run = mode == CAL_MODE_SWEEP;
  s_status.levels_run = mode == CAL_MODE_LEVELS;
  s_status.state = GW_CAL_BUSY;
  xSemaphoreGive(s_lock);
  if (xTaskCreatePinnedToCore(calib_task, "gw_calib", CAL_TASK_STACK,
                              (void *)(uintptr_t)mode, CAL_TASK_PRIORITY,
                              NULL, 1) != pdPASS) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_running = false;
    s_status.state = GW_CAL_IDLE;
    xSemaphoreGive(s_lock);
    return ESP_ERR_NO_MEM;
  }
  return ESP_OK;
#endif
}

esp_err_t gw_calib_start(bool mic_test_only) {
  return start_mode(mic_test_only ? CAL_MODE_TEST : CAL_MODE_FULL);
}

/* A connected speaker's role, false if it is not connected. */
static bool connected_role(const uint8_t mac[6], int8_t *channel) {
  gw_speaker_info_t *list = malloc(sizeof(gw_speaker_info_t) * SURROUND_MAX_SPEAKERS);
  if (!list) return false;
  const size_t n = gw_speakers_list(list);
  bool found = false;
  for (size_t i = 0; i < n && !found; ++i) {
    found = list[i].config_valid && memcmp(list[i].mac, mac, 6) == 0;
    if (found && channel) *channel = list[i].config.channel;
  }
  free(list);
  return found;
}

/* ---- The level limit of each speaker, in the Gateway's NVS ---- */
#define CAL_CAP_NS "gw_calib"

static void cap_key(const uint8_t mac[6], char key[16]) {
  snprintf(key, 16, "c%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

int16_t gw_calib_cap_get(const uint8_t mac[6]) {
  int16_t v = GW_CAL_CAP_DEFAULT_DB10;
  nvs_handle_t h;
  if (mac && nvs_open(CAL_CAP_NS, NVS_READONLY, &h) == ESP_OK) {
    char key[16];
    cap_key(mac, key);
    int16_t got = 0;
    if (nvs_get_i16(h, key, &got) == ESP_OK && got >= GW_CAL_LEVEL_MIN_DB10 &&
        got <= GW_CAL_LEVEL_MAX_DB10)
      v = got;
    nvs_close(h);
  }
  return v;
}

esp_err_t gw_calib_cap_set(const uint8_t mac[6], int16_t cap_db10) {
  if (!mac || cap_db10 < GW_CAL_LEVEL_MIN_DB10 || cap_db10 > GW_CAL_LEVEL_MAX_DB10)
    return ESP_ERR_INVALID_ARG;
  nvs_handle_t h;
  esp_err_t err = nvs_open(CAL_CAP_NS, NVS_READWRITE, &h);
  if (err != ESP_OK) return err;
  char key[16];
  cap_key(mac, key);
  err = nvs_set_i16(h, key, cap_db10);
  if (err == ESP_OK) err = nvs_commit(h);
  nvs_close(h);
  AP_LOGI(TAG, "Room EQ level limit of %02x:%02x:%02x:%02x:%02x:%02x: %.1f dBFS RMS (%s)",
          mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], cap_db10 / 10.0, esp_err_to_name(err));
  return err;
}

/* The level asked against the range and the speaker's limit. */
static esp_err_t level_allowed(const uint8_t mac[6], int16_t level_db10) {
  if (level_db10 < GW_CAL_LEVEL_MIN_DB10 || level_db10 > GW_CAL_LEVEL_MAX_DB10)
    return ESP_ERR_INVALID_ARG;
  return level_db10 > gw_calib_cap_get(mac) ? ESP_ERR_INVALID_SIZE : ESP_OK;
}

esp_err_t gw_calib_sweep_start(const uint8_t mac[6], bool raw, bool per_way,
                               const gw_cal_sweep_opts_t *opts) {
  if (!mac || !opts || opts->repeats < 1U || opts->repeats > GW_CAL_REPEATS_MAX)
    return ESP_ERR_INVALID_ARG;
  int8_t channel = -1;
  if (!connected_role(mac, &channel)) return ESP_ERR_NOT_FOUND;
  const esp_err_t lv = level_allowed(mac, opts->level_db10);
  if (lv != ESP_OK) return lv;
  const bool ways = per_way && channel != CAL_AAC_SPEAKER_LFE;
  const int sweeps = (ways ? (opts->all_ways ? 3 : 2) : 1) * (int)opts->repeats;
  cal_sweep_def_t def;
  uint32_t play_ms = 0;
  if (sweeps > SURROUND_CAL_BURSTS_MAX || !sweep_plan(opts, sweeps, &def, &play_ms))
    return ESP_ERR_INVALID_ARG;
  if (play_ms + 2000U > GW_CAL_RECORD_MAX_S * 1000U) return ESP_ERR_INVALID_SIZE;
  if (gw_calib_running()) return ESP_ERR_INVALID_STATE;
  /* Read by the task this start creates, after it. */
  memcpy(s_sweep_req.mac, mac, sizeof(s_sweep_req.mac));
  s_sweep_req.raw = raw;
  s_sweep_req.per_way = per_way;
  s_sweep_req.opts = *opts;
  return start_mode(CAL_MODE_SWEEP);
}

esp_err_t gw_calib_levels_start(const uint8_t mac[6], int16_t level_db10) {
  if (!mac) return ESP_ERR_INVALID_ARG;
  if (!connected_role(mac, NULL)) return ESP_ERR_NOT_FOUND;
  const esp_err_t lv = level_allowed(mac, level_db10);
  if (lv != ESP_OK) return lv;
  if (gw_calib_running()) return ESP_ERR_INVALID_STATE;
  memcpy(s_sweep_req.mac, mac, sizeof(s_sweep_req.mac));
  memset(&s_sweep_req.opts, 0, sizeof(s_sweep_req.opts));
  s_sweep_req.opts.level_db10 = level_db10;
  return start_mode(CAL_MODE_LEVELS);
}

bool gw_calib_sweep_samples(uint32_t id, const int16_t **samples, gw_cal_sweep_t *info) {
  if (!init_once() || !samples || !info) return false;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  const bool ok = !s_running && s_status.sweep_run && s_status.sweep.valid &&
                  s_status.sweep.id == id;
  if (ok) {
    *info = s_status.sweep;
    s_sweep_readers++;
  }
  xSemaphoreGive(s_lock);
  if (!ok) return false;
  uint32_t n = 0;
  const int16_t *x = gw_mic_samples(&n);
  if (!x || info->from + info->count > n) { /* released for the player or the radio */
    gw_calib_sweep_done();
    return false;
  }
  *samples = x + info->from;
  return true;
}

void gw_calib_sweep_done(void) {
  if (!init_once()) return;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  if (s_sweep_readers > 0) s_sweep_readers--;
  xSemaphoreGive(s_lock);
}

void gw_calib_release_recording(void) {
  if (!init_once()) return;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  if (!s_running && s_sweep_readers == 0) gw_mic_release();
  xSemaphoreGive(s_lock);
}

bool gw_calib_running(void) { return atomic_load_explicit(&s_running, memory_order_relaxed); }

void gw_calib_get(gw_cal_status_t *out) {
  if (!init_once()) {
    memset(out, 0, sizeof(*out));
    return;
  }
  xSemaphoreTake(s_lock, portMAX_DELAY);
  *out = s_status;
  /* Completion includes restoring AirPlay, not just the last DSP result. */
  if (s_running) out->state = GW_CAL_BUSY;
  xSemaphoreGive(s_lock);
#if defined(CONFIG_AIRPLAY_SURROUND_CAL_MIC) && CONFIG_AIRPLAY_SURROUND_CAL_MIC
  out->mic_built = true;
#endif
}

esp_err_t gw_calib_apply(int *applied) {
  if (applied) *applied = 0;
  if (!init_once()) return ESP_ERR_NO_MEM;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  if (s_running || s_status.state != GW_CAL_DONE || s_status.mic_test ||
      s_status.sweep_run) {
    xSemaphoreGive(s_lock);
    return ESP_ERR_INVALID_STATE;
  }
  gw_cal_status_t st = s_status;
  xSemaphoreGive(s_lock);
  int n = 0;
  for (int i = 0; i < st.count; ++i) {
    const gw_cal_speaker_t *o = &st.spk[i];
    if (!o->has_suggestion) continue;
    surround_set_config_t cfg = {
        .fields = SURROUND_SET_DELAY,
        .delay_us = (int32_t)lroundf(o->delay_ms * 1000.0f),
    };
    if (o->has_trim) {
      cfg.fields |= SURROUND_SET_TRIM;
      cfg.trim_db10 = (int16_t)lroundf(o->trim_db * 10.0f);
    }
    if (gw_speakers_set_config(o->mac, &cfg) == ESP_OK) n++;
  }
  AP_LOGI(TAG, "calibration applied to %d speakers", n);
  if (applied) *applied = n;
  gw_calib_discard();
  return n > 0 ? ESP_OK : ESP_ERR_NOT_FOUND;
}

void gw_calib_discard(void) {
  if (!init_once()) return;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  if (!s_running) {
    memset(&s_status, 0, sizeof(s_status));
  }
  xSemaphoreGive(s_lock);
}
