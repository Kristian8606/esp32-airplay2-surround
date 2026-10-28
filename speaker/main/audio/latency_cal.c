/* ADC loopback capture for the wired latency measurement (ESP32-S3).
 * Compiled only with CONFIG_AIRPLAY_LATENCY_CAL. Idle unless the web UI asks
 * for a measurement; allocates everything on start and frees it on finish. */
#include "latency_cal.h"

#include <string.h>

#include "esp_adc/adc_continuous.h"
#include "soc/soc_caps.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "airplay_log.h"

static const char *TAG = "latency_cal";

#define CAP_SECONDS_X10      17   /* 1.7 s of samples */
#define CAP_MAX_SAMPLES      (LATENCY_CAL_ADC_RATE_HZ * CAP_SECONDS_X10 / 10)
#define FRAME_CONV           256  /* conversions per ADC DMA frame */
#define MAX_FRAMES           (CAP_MAX_SAMPLES / FRAME_CONV + 64)
#define READ_CHUNK_BYTES     (FRAME_CONV * SOC_ADC_DIGI_RESULT_BYTES)

typedef struct {
  adc_continuous_handle_t adc;
  TaskHandle_t reader;
  volatile bool running;
  volatile bool overflow;
  uint16_t *samples;          /* PSRAM */
  volatile uint32_t n;
  /* ISR-written frame stamps (internal RAM) */
  uint32_t *cb_count;
  int64_t *cb_t;
  volatile uint32_t cb_n;
  volatile uint32_t conv_total;
  uint8_t *rdbuf;             /* reader buffer (heap, NOT on the task stack) */
  bool started;               /* adc_continuous_start() succeeded */
} cap_t;

static cap_t s_cap;

static bool IRAM_ATTR on_conv_done(adc_continuous_handle_t handle,
                                   const adc_continuous_evt_data_t *edata,
                                   void *user_data) {
  (void)handle;
  cap_t *c = (cap_t *)user_data;
  c->conv_total += edata->size / SOC_ADC_DIGI_RESULT_BYTES;
  const uint32_t k = c->cb_n;
  if (k < MAX_FRAMES) {
    c->cb_count[k] = c->conv_total;
    c->cb_t[k] = esp_timer_get_time();
    c->cb_n = k + 1;
  }
  return false;
}

static bool IRAM_ATTR on_pool_ovf(adc_continuous_handle_t handle,
                                  const adc_continuous_evt_data_t *edata,
                                  void *user_data) {
  (void)handle; (void)edata;
  ((cap_t *)user_data)->overflow = true;
  return false;
}

static void reader_task(void *arg) {
  cap_t *c = (cap_t *)arg;
  uint8_t *buf = c->rdbuf;
  while (c->running) {
    uint32_t got = 0;
    const esp_err_t err = adc_continuous_read(c->adc, buf, READ_CHUNK_BYTES, &got, 20);
    if (err == ESP_ERR_TIMEOUT) continue;
    if (err != ESP_OK) {
      /* Never spin on an error. The driver logs every failed read, and a
       * tight loop of those logs overflows this task's stack. */
      const TickType_t delay = pdMS_TO_TICKS(5);
      vTaskDelay(delay ? delay : 1);
      continue;
    }
    for (uint32_t i = 0; i + SOC_ADC_DIGI_RESULT_BYTES <= got;
         i += SOC_ADC_DIGI_RESULT_BYTES) {
      const adc_digi_output_data_t *p = (const adc_digi_output_data_t *)&buf[i];
      /* Single-channel pattern: every conversion is ours. Keep all of them so
       * sample indices stay aligned with the frame stamps. */
      if (c->n < CAP_MAX_SAMPLES) c->samples[c->n++] = (uint16_t)p->type2.data;
    }
  }
  c->reader = NULL;
  vTaskDelete(NULL);
}

static void cap_free(void) {
  if (s_cap.adc) {
    if (s_cap.started) (void)adc_continuous_stop(s_cap.adc);
    s_cap.started = false;
    (void)adc_continuous_deinit(s_cap.adc);
    s_cap.adc = NULL;
  }
  heap_caps_free(s_cap.samples);
  heap_caps_free(s_cap.cb_count);
  heap_caps_free(s_cap.cb_t);
  heap_caps_free(s_cap.rdbuf);
  s_cap.samples = NULL; s_cap.cb_count = NULL; s_cap.cb_t = NULL;
  s_cap.rdbuf = NULL;
}

esp_err_t latency_cal_capture_start(void) {
  if (s_cap.running || s_cap.adc) return ESP_ERR_INVALID_STATE;
  memset(&s_cap, 0, sizeof(s_cap));

  adc_unit_t unit;
  adc_channel_t channel;
  esp_err_t err = adc_continuous_io_to_channel(CONFIG_AIRPLAY_LATENCY_CAL_GPIO,
                                               &unit, &channel);
  if (err != ESP_OK || unit != ADC_UNIT_1) {
    ESP_LOGE(TAG, "GPIO%d is not an ADC1 pin", CONFIG_AIRPLAY_LATENCY_CAL_GPIO);
    return ESP_ERR_INVALID_ARG;
  }

  s_cap.samples = heap_caps_malloc(sizeof(uint16_t) * CAP_MAX_SAMPLES,
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  s_cap.cb_count = heap_caps_malloc(sizeof(uint32_t) * MAX_FRAMES,
                                    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  s_cap.cb_t = heap_caps_malloc(sizeof(int64_t) * MAX_FRAMES,
                                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  s_cap.rdbuf = heap_caps_malloc(READ_CHUNK_BYTES,
                                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!s_cap.samples || !s_cap.cb_count || !s_cap.cb_t || !s_cap.rdbuf) {
    cap_free();
    return ESP_ERR_NO_MEM;
  }

  /* The IDF driver aborts inside its own error path when it cannot
   * get internal DMA memory (seen with an AirPlay session open). Check first
   * and fail cleanly instead. */
  if (heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL) <
      READ_CHUNK_BYTES * 16 + 8192) {
    ESP_LOGE(TAG, "not enough internal DMA memory for the ADC");
    cap_free();
    return ESP_ERR_NO_MEM;
  }

  const adc_continuous_handle_cfg_t hcfg = {
      .max_store_buf_size = READ_CHUNK_BYTES * 16,
      .conv_frame_size = READ_CHUNK_BYTES,
  };
  err = adc_continuous_new_handle(&hcfg, &s_cap.adc);
  if (err != ESP_OK) { cap_free(); return err; }

  adc_digi_pattern_config_t pattern = {
      .atten = ADC_ATTEN_DB_12,
      .channel = (uint8_t)(channel & 0x7),
      .unit = (uint8_t)unit,
      .bit_width = SOC_ADC_DIGI_MAX_BITWIDTH,
  };
  const adc_continuous_config_t cfg = {
      .pattern_num = 1,
      .adc_pattern = &pattern,
      .sample_freq_hz = LATENCY_CAL_ADC_RATE_HZ,
      .conv_mode = ADC_CONV_SINGLE_UNIT_1,
      .format = ADC_DIGI_OUTPUT_FORMAT_TYPE2,
  };
  err = adc_continuous_config(s_cap.adc, &cfg);
  if (err == ESP_OK) {
    const adc_continuous_evt_cbs_t cbs = {
        .on_conv_done = on_conv_done,
        .on_pool_ovf = on_pool_ovf,
    };
    err = adc_continuous_register_event_callbacks(s_cap.adc, &cbs, &s_cap);
  }
  if (err != ESP_OK) { cap_free(); return err; }

  /* Start the ADC BEFORE the reader; otherwise the reader hits "driver is
   * already stopped" on every read. */
  err = adc_continuous_start(s_cap.adc);
  if (err != ESP_OK) { cap_free(); return err; }
  s_cap.started = true;
  s_cap.running = true;
  /* 4 KiB: the IDF driver may log from inside adc_continuous_read(), and the
   * log path (vsnprintf + web log stream hook) needs real stack. */
  if (xTaskCreatePinnedToCore(reader_task, "lat_cal_adc", 4096, &s_cap, 6,
                              &s_cap.reader, 0) != pdPASS) {
    s_cap.running = false;
    cap_free();
    return ESP_ERR_NO_MEM;
  }
  AP_LOGI(TAG, "loopback capture started on GPIO%d (%u Hz)",
           CONFIG_AIRPLAY_LATENCY_CAL_GPIO, (unsigned)LATENCY_CAL_ADC_RATE_HZ);
  return ESP_OK;
}

void latency_cal_capture_finish(const int64_t *emit_us, int n_emit,
                                latency_cal_result_t *res) {
  memset(res, 0, sizeof(*res));
  if (!s_cap.adc) { res->error = "capture not running"; return; }
  s_cap.running = false;
  for (int i = 0; s_cap.reader && i < 100; ++i) vTaskDelay(pdMS_TO_TICKS(10));
  if (s_cap.started) (void)adc_continuous_stop(s_cap.adc);
  s_cap.started = false;

  if (s_cap.overflow) {
    res->error = "ADC buffer overflow (CPU too busy)";
  } else if (s_cap.reader) {
    res->error = "ADC reader did not stop";
  } else {
    latency_cal_analyze(s_cap.samples, s_cap.n, s_cap.cb_count, s_cap.cb_t,
                        s_cap.cb_n, emit_us, n_emit, res);
  }
  AP_LOGI(TAG, "capture: %lu samples, %lu frames -> %s",
           (unsigned long)s_cap.n, (unsigned long)s_cap.cb_n,
           res->ok ? "ok" : (res->error ? res->error : "failed"));
  if (!s_cap.reader) cap_free();
}
