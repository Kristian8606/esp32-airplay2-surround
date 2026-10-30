#include "gw_calib.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "airplay_log.h"
#include "audio_receiver.h"
#include "cal_chirp.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gw_mic.h"
#include "gw_speakers.h"
#include "rtsp_server.h"

static const char *TAG = "gw_calib";

#define CAL_TASK_STACK      6144U
#define CAL_TASK_PRIORITY   3
#define CAL_NOISE_MS        1000
#define CAL_RECORD_MAX_S    10U
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
/* Levels tried, quietest first, until the chirp is heard clearly. */
static const int8_t k_levels_db[] = {-30, -24, -18, -12};
/* Without a microphone the chirps are only played once, at this level. */
#define CAL_PLAY_ONLY_DB    (-24)
#define CAL_AAC_SPEAKER_LFE 3 /* aac_speaker_t of the subwoofer */

static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_result_sem;
static gw_cal_status_t s_status;
static bool s_running;
/* The CAL_RESULT being waited for (gw_calib_on_result). */
static uint8_t s_wait_mac[6];
static uint8_t s_wait_seq;
static bool s_waiting;
static surround_cal_result_t s_result;
static uint8_t s_seq;

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
  if (!s_lock || !s_waiting || r->seq != s_wait_seq ||
      memcmp(mac, s_wait_mac, sizeof(s_wait_mac)) != 0)
    return;
  s_result = *r;
  s_waiting = false;
  xSemaphoreGive(s_result_sem);
}

static float to_db(double v) { return v > 1e-12 ? (float)(20.0 * log10(v)) : -240.0f; }

/* Room noise from a short recording; false when the microphone gives
 * nothing usable. */
static bool measure_noise(float *rms_dbfs, float *peak_dbfs, const char **why) {
  if (gw_mic_start(2U) != ESP_OK) {
    *why = "The microphone could not start";
    return false;
  }
  vTaskDelay(pdMS_TO_TICKS(CAL_NOISE_MS));
  gw_mic_stop();
  uint32_t n = 0;
  const float *x = gw_mic_samples(&n);
  const uint32_t skip = GW_MIC_RATE_HZ / 10U; /* the microphone's start-up */
  if (!x || n <= skip + GW_MIC_RATE_HZ / 4U) {
    *why = "No data from the microphone";
    return false;
  }
  double mean = 0.0;
  for (uint32_t i = skip; i < n; ++i) mean += x[i];
  mean /= (double)(n - skip);
  double sq = 0.0, peak = 0.0;
  for (uint32_t i = skip; i < n; ++i) {
    const double v = x[i] - mean;
    sq += v * v;
    if (fabs(v) > peak) peak = fabs(v);
  }
  *rms_dbfs = to_db(sqrt(sq / (double)(n - skip)));
  *peak_dbfs = to_db(peak);
  if (*rms_dbfs < -110.0f) {
    *why = "No signal from the microphone: check its wiring";
    return false;
  }
  return true;
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
  double amplitude;
} chirp_hit_t;

static double correlate(const float *x, const float *ref, uint32_t len) {
  float acc = 0.0f;
  for (uint32_t j = 0; j < len; ++j) acc += x[j] * ref[j];
  return acc;
}

/* Matched filter over x[0..n): the best fit of the reference. */
static chirp_hit_t find_chirp(const float *x, uint32_t n, const float *ref,
                              uint32_t len, double ref_energy) {
  chirp_hit_t hit = {0};
  if (n <= len + 2U) return hit;
  const uint32_t lags = n - len;
  double ex = 0.0; /* energy of x under the reference, sliding */
  for (uint32_t j = 0; j < len; ++j) ex += (double)x[j] * x[j];
  double best = 0.0, best_c = 0.0;
  uint32_t best_lag = 0;
  for (uint32_t lag = 0; lag <= lags; ++lag) {
    if (lag > 0) {
      ex += (double)x[lag + len - 1] * x[lag + len - 1] -
            (double)x[lag - 1] * x[lag - 1];
    }
    const double c = correlate(x + lag, ref, len);
    const double norm = ex > 0.0 ? c / sqrt(ex * ref_energy) : 0.0;
    if (fabs(norm) > fabs(best)) {
      best = norm;
      best_c = c;
      best_lag = lag;
    }
  }
  hit.found = best != 0.0;
  hit.match = (float)best;
  hit.amplitude = fabs(best_c) / ref_energy;
  /* A parabola through the peak and its neighbours: sub-sample timing. */
  double frac = 0.0;
  if (best_lag > 0U && best_lag < lags) {
    const double a = fabs(correlate(x + best_lag - 1U, ref, len));
    const double b = fabs(best_c);
    const double c = fabs(correlate(x + best_lag + 1U, ref, len));
    const double den = a - 2.0 * b + c;
    if (den < 0.0) frac = 0.5 * (a - c) / den;
    if (frac < -0.5 || frac > 0.5) frac = 0.0;
  }
  hit.index = (double)best_lag + frac;
  return hit;
}

static int cmp_double(const void *a, const void *b) {
  const double x = *(const double *)a, y = *(const double *)b;
  return (x > y) - (x < y);
}

static double median(double *v, int n) {
  qsort(v, (size_t)n, sizeof(double), cmp_double);
  return n % 2 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

/* One run of chirps from one speaker at one level. */
/* Ask one speaker for its chirps and wait until it has played them; the
 * answer is in s_result. False (with a note) when it did not play. */
static bool request_chirps(gw_cal_speaker_t *o, cal_chirp_kind_t kind,
                           int8_t level_db) {
  if (++s_seq == 0U) ++s_seq;
  const surround_cal_play_t m = {.seq = s_seq,
                                 .kind = (uint8_t)kind,
                                 .level_db = level_db,
                                 .bursts = CAL_CHIRP_BURSTS};
  (void)xSemaphoreTake(s_result_sem, 0);
  memcpy(s_wait_mac, o->mac, sizeof(s_wait_mac));
  s_wait_seq = s_seq;
  s_waiting = true;
  const esp_err_t send_err = gw_speakers_cal_play(o->mac, &m);
  const bool answered =
      send_err == ESP_OK &&
      xSemaphoreTake(s_result_sem, pdMS_TO_TICKS(CAL_RESULT_WAIT_MS)) == pdTRUE;
  s_waiting = false;
  if (!answered) {
    snprintf(o->note, sizeof(o->note), "%s",
             send_err == ESP_ERR_NOT_FOUND ? "Not connected" : "No answer from the speaker");
    return false;
  }
  if (s_result.err != ESP_OK || s_result.n == 0U) {
    snprintf(o->note, sizeof(o->note), "The speaker could not play: %s",
             esp_err_to_name(s_result.err));
    return false;
  }
  return true;
}

/* Without a microphone: the speaker still plays, so the chirps can be heard,
 * but nothing is measured. */
static void play_only(gw_cal_speaker_t *o, cal_chirp_kind_t kind) {
  if (request_chirps(o, kind, CAL_PLAY_ONLY_DB)) {
    snprintf(o->note, sizeof(o->note), "Played its test chirps; not measured");
  }
}

static bool measure_once(gw_cal_speaker_t *o, cal_chirp_kind_t kind,
                         int8_t level_db) {
  if (gw_mic_start(CAL_RECORD_MAX_S) != ESP_OK) {
    snprintf(o->note, sizeof(o->note), "The microphone could not start");
    return false;
  }
  vTaskDelay(pdMS_TO_TICKS(50));
  const bool played = request_chirps(o, kind, level_db);
  vTaskDelay(pdMS_TO_TICKS(100));
  gw_mic_stop();
  if (!played) return false;

  uint32_t n = 0;
  const float *x = gw_mic_samples(&n);
  const bool low = kind == CAL_CHIRP_LOW;
  const uint32_t dec = low ? CAL_LOW_DECIMATE : 1U;
  const uint32_t rate = GW_MIC_RATE_HZ / dec;
  uint32_t len = 0;
  double energy = 0.0;
  float *ref = make_reference(kind, rate, &len, &energy);
  const uint32_t win = (uint32_t)ceil(
      ((low ? CAL_SEARCH_LOW_S : CAL_SEARCH_WIDE_S) + CAL_SEARCH_EARLY_S) * rate) + len;
  float *seg = heap_caps_malloc(win * sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  double tof[SURROUND_CAL_BURSTS_MAX], amp[SURROUND_CAL_BURSTS_MAX],
      match[SURROUND_CAL_BURSTS_MAX];
  int hits = 0, inverted = 0;
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
    /* The analysis segment, the subwoofer's averaged down to its rate. */
    for (uint32_t i = 0; i < win; ++i) {
      float acc = 0.0f;
      for (uint32_t d = 0; d < dec; ++d) acc += x[s0 + i * dec + d];
      seg[i] = acc / (float)dec;
    }
    const chirp_hit_t hit = find_chirp(seg, win, ref, len, energy);
    if (!hit.found) continue;
    int64_t arrival_local = 0;
    if (!gw_mic_sample_time_ns((double)s0 + hit.index * dec, &arrival_local)) continue;
    tof[hits] = (double)(arrival_local - emit_local) / 1e6;
    amp[hits] = hit.amplitude;
    match[hits] = fabs(hit.match);
    if (hit.match < 0.0f) inverted++;
    hits++;
  }
  heap_caps_free(seg);
  heap_caps_free(ref);
  if (hits == 0) {
    snprintf(o->note, sizeof(o->note), "Not found in the recording");
    return false;
  }
  double tmin = tof[0], tmax = tof[0];
  for (int i = 1; i < hits; ++i) {
    if (tof[i] < tmin) tmin = tof[i];
    if (tof[i] > tmax) tmax = tof[i];
  }
  o->low = low;
  o->tof_ms = (float)median(tof, hits);
  o->distance_m = o->tof_ms * CAL_SOUND_M_PER_MS;
  o->level_db = to_db(median(amp, hits)) - (float)level_db;
  o->match = (float)median(match, hits);
  o->spread_ms = (float)(tmax - tmin);
  o->inverted = !low && inverted * 2 > hits;
  const float max_spread = low ? CAL_MAX_SPREAD_LOW_MS : CAL_MAX_SPREAD_WIDE_MS;
  o->measured = o->match >= CAL_MIN_MATCH && o->spread_ms <= max_spread &&
                o->tof_ms > -1.0f;
  if (!o->measured) {
    snprintf(o->note, sizeof(o->note), "Not heard clearly (match %.2f)", (double)o->match);
  } else {
    o->note[0] = '\0';
  }
  AP_LOGI(TAG,
          "%s at %d dBFS: %d/%d chirps, travel %.2f ms (%.2f m) spread %.2f ms, "
          "level %.1f dB, match %.2f%s",
          o->name, level_db, hits, s_result.n, (double)o->tof_ms,
          (double)o->distance_m, (double)o->spread_ms, (double)o->level_db,
          (double)o->match, o->inverted ? ", inverted" : "");
  return o->measured;
}

/* Delays: the farthest speaker 0 ms, the others as much later as they are
 * closer. Levels: the quietest 0 dB, the others turned down to it. The
 * subwoofer's chirp is low and its travel time less exact, so it gets a
 * delay but no level, and a note to check it by ear. */
static void suggest(void) {
  float tof_max = -1e9f, level_min = 1e9f;
  for (int i = 0; i < s_status.count; ++i) {
    const gw_cal_speaker_t *o = &s_status.spk[i];
    if (!o->measured || o->low) continue;
    if (o->tof_ms > tof_max) tof_max = o->tof_ms;
    if (o->level_db < level_min) level_min = o->level_db;
  }
  if (tof_max < -1e8f) return;
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
    } else {
      float t = level_min - o->level_db;
      if (t < SURROUND_TRIM_MIN_DB10 / 10.0f) t = SURROUND_TRIM_MIN_DB10 / 10.0f;
      o->trim_db = roundf(t * 2.0f) / 2.0f;
      o->has_trim = true;
      if (o->inverted) snprintf(o->note, sizeof(o->note), "Polarity looks inverted: check + and -");
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
    if (rtsp_server_is_idle() && audio_receiver_is_idle()) return true;
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  return rtsp_server_is_idle() && audio_receiver_is_idle();
}

static void store_noise(bool valid, float rms, float peak) {
  xSemaphoreTake(s_lock, portMAX_DELAY);
  s_status.noise_valid = valid;
  s_status.noise_dbfs = rms;
  s_status.peak_dbfs = peak;
  xSemaphoreGive(s_lock);
}

/* Every speaker in turn: measured with the microphone, or only played. */
static void run_speakers(bool mic_ok, const char *mic_why) {
  static gw_speaker_info_t list[SURROUND_MAX_SPEAKERS];
  const size_t n = gw_speakers_list(list);
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
      for (size_t l = 0; l < sizeof(k_levels_db); ++l) {
        if (measure_once(&o, kind, k_levels_db[l])) break;
        if (!audio_receiver_is_idle()) break; /* a stream started */
      }
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
  xSemaphoreTake(s_lock, portMAX_DELAY);
  suggest();
  int ok = 0;
  for (int i = 0; i < s_status.count; ++i) ok += s_status.spk[i].measured;
  xSemaphoreGive(s_lock);
  set_message(ok ? GW_CAL_DONE : GW_CAL_FAILED,
              ok ? "Done: %d of %u speakers measured. Nothing changes until you apply."
                 : "No speaker was heard clearly (%d of %u)",
              ok, (unsigned)count);
}

static void calib_task(void *arg) {
  const bool mic_test = (bool)(uintptr_t)arg;
  bool stopped_airplay = false;
  float rms = 0.0f, peak = 0.0f;
  const char *why = NULL;
  /* 1. Is there a microphone at all? This works while music plays, so the
   *    result is known before the sender is disconnected. */
  set_message(GW_CAL_BUSY, "Checking the microphone");
  bool mic_ok = measure_noise(&rms, &peak, &why);
  if (mic_test) {
    store_noise(mic_ok, rms, peak);
    if (mic_ok) {
      set_message(GW_CAL_DONE, "Microphone works; room noise %.0f dBFS", (double)rms);
    } else {
      set_message(GW_CAL_FAILED, "%s", why);
    }
  } else if (!audio_receiver_is_idle() && !(stopped_airplay = stop_sender())) {
    /* 2. The speakers only chirp between streams. */
    set_message(GW_CAL_FAILED, "The AirPlay sender did not let go; try again");
  } else {
    /* 3. The noise of the quiet room, then the speakers. Without a
     *    microphone they still play, so the procedure can be heard. */
    if (mic_ok) {
      set_message(GW_CAL_BUSY, "Listening to the room");
      mic_ok = measure_noise(&rms, &peak, &why);
    }
    store_noise(mic_ok, rms, peak);
    run_speakers(mic_ok, why);
  }
  if (stopped_airplay || !rtsp_server_is_idle()) {
    /* AirPlay again; also after a stop that timed out. */
    if (rtsp_server_is_idle()) {
      const esp_err_t err = rtsp_server_start();
      if (err != ESP_OK) ESP_LOGE(TAG, "AirPlay restart failed: %s", esp_err_to_name(err));
    }
  }
  xSemaphoreTake(s_lock, portMAX_DELAY);
  s_status.sender_stopped = stopped_airplay;
  s_running = false;
  xSemaphoreGive(s_lock);
  vTaskDelete(NULL);
}

static bool init_once(void) {
  if (s_lock) return true;
  s_lock = xSemaphoreCreateMutex();
  s_result_sem = xSemaphoreCreateBinary();
  return s_lock && s_result_sem;
}

esp_err_t gw_calib_start(bool mic_test_only) {
#if !(defined(CONFIG_AIRPLAY_SURROUND_CAL_MIC) && CONFIG_AIRPLAY_SURROUND_CAL_MIC)
  (void)mic_test_only;
  return ESP_ERR_NOT_SUPPORTED;
#else
  if (!init_once()) return ESP_ERR_NO_MEM;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  if (s_running) {
    xSemaphoreGive(s_lock);
    return ESP_ERR_INVALID_STATE;
  }
  s_running = true;
  memset(&s_status, 0, sizeof(s_status));
  s_status.mic_built = true;
  s_status.mic_test = mic_test_only;
  s_status.state = GW_CAL_BUSY;
  xSemaphoreGive(s_lock);
  if (xTaskCreatePinnedToCore(calib_task, "gw_calib", CAL_TASK_STACK,
                              (void *)(uintptr_t)mic_test_only, CAL_TASK_PRIORITY,
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

bool gw_calib_running(void) { return __atomic_load_n(&s_running, __ATOMIC_RELAXED); }

void gw_calib_get(gw_cal_status_t *out) {
  if (!init_once()) {
    memset(out, 0, sizeof(*out));
    return;
  }
  xSemaphoreTake(s_lock, portMAX_DELAY);
  *out = s_status;
  xSemaphoreGive(s_lock);
#if defined(CONFIG_AIRPLAY_SURROUND_CAL_MIC) && CONFIG_AIRPLAY_SURROUND_CAL_MIC
  out->mic_built = true;
#endif
}

esp_err_t gw_calib_apply(int *applied) {
  if (applied) *applied = 0;
  if (!init_once()) return ESP_ERR_NO_MEM;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  if (s_running || s_status.state != GW_CAL_DONE || s_status.mic_test) {
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
