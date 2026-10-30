#include "gw_mic.h"

#include "sdkconfig.h"

#if defined(CONFIG_AIRPLAY_SURROUND_CAL_MIC) && CONFIG_AIRPLAY_SURROUND_CAL_MIC

#include <string.h>

#include "driver/i2s_std.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "gw_mic";

#define MIC_DMA_DESC      6U
#define MIC_DMA_FRAMES    240U  /* 5 ms per DMA buffer */
#define MIC_READ_SAMPLES  512U
#define MIC_MAX_STAMPS    2048U /* DMA completions: 10 s */
#define MIC_TASK_STACK    3072U
#define MIC_TASK_PRIORITY 6

static i2s_chan_handle_t s_rx;
static TaskHandle_t s_task;
static volatile bool s_running;
static float *s_samples;  /* PSRAM */
static uint32_t s_capacity;
static volatile uint32_t s_count;
/* Written by the receive interrupt: samples received so far and when. */
static uint32_t *s_stamp_count;
static int64_t *s_stamp_us;
static volatile uint32_t s_stamps;
static volatile uint32_t s_dma_samples;
/* DMA buffers the driver dropped because the reader was behind: the
 * recording no longer matches its DMA times. */
static volatile uint32_t s_overflows;
/* Sample time line of the last recording: t_us = a + b * (index + 1). */
static double s_a_us, s_b_us;
static bool s_line_valid;

static bool IRAM_ATTR on_recv(i2s_chan_handle_t handle, i2s_event_data_t *event,
                              void *ctx) {
  (void)handle;
  (void)ctx;
  s_dma_samples += (uint32_t)(event->size / sizeof(int32_t));
  const uint32_t n = s_stamps;
  if (n < MIC_MAX_STAMPS) {
    s_stamp_count[n] = s_dma_samples;
    s_stamp_us[n] = esp_timer_get_time();
    s_stamps = n + 1U;
  }
  return false;
}

static bool IRAM_ATTR on_recv_q_ovf(i2s_chan_handle_t handle,
                                    i2s_event_data_t *event, void *ctx) {
  (void)handle;
  (void)event;
  (void)ctx;
  s_overflows++;
  return false;
}

static void reader_task(void *arg) {
  (void)arg;
  static int32_t chunk[MIC_READ_SAMPLES];
  while (s_running) {
    size_t got = 0;
    /* The timeout is in milliseconds, not ticks. On a timeout the samples
     * read so far are still kept: every sample the DMA counted must be in
     * the recording, or its times no longer match. */
    const esp_err_t err =
        i2s_channel_read(s_rx, chunk, sizeof(chunk), &got, 100);
    if (err != ESP_OK && err != ESP_ERR_TIMEOUT) continue;
    const uint32_t n = (uint32_t)(got / sizeof(int32_t));
    for (uint32_t i = 0; i < n && s_count < s_capacity; ++i) {
      /* INMP441: 24 bits, MSB first, in a 32-bit slot. */
      s_samples[s_count++] = (float)(chunk[i] >> 8) / 8388608.0f;
    }
  }
  s_task = NULL;
  vTaskDelete(NULL);
}

/* A line through the DMA completion times. The interrupt is only ever late,
 * so the line moves down to its lower envelope (as latency_cal.c does). */
static void fit_line(void) {
  s_line_valid = false;
  const uint32_t n = s_stamps;
  if (n < 10U) return;
  double sx = 0, sy = 0, sxx = 0, sxy = 0;
  const double c0 = (double)s_stamp_count[0], t0 = (double)s_stamp_us[0];
  for (uint32_t k = 0; k < n; ++k) {
    const double x = (double)s_stamp_count[k] - c0;
    const double y = (double)s_stamp_us[k] - t0;
    sx += x;
    sy += y;
    sxx += x * x;
    sxy += x * y;
  }
  const double den = (double)n * sxx - sx * sx;
  if (den <= 0.0) return;
  const double b = ((double)n * sxy - sx * sy) / den;
  double a = t0 + (sy - b * sx) / (double)n - b * c0;
  double min_res = 0.0;
  for (uint32_t k = 0; k < n; ++k) {
    const double r = (double)s_stamp_us[k] - (a + b * (double)s_stamp_count[k]);
    if (k == 0 || r < min_res) min_res = r;
  }
  s_a_us = a + min_res;
  s_b_us = b;
  s_line_valid = b > 0.0;
}

esp_err_t gw_mic_start(uint32_t max_seconds) {
  if (s_running || s_task) return ESP_ERR_INVALID_STATE;
  const uint32_t capacity = max_seconds * GW_MIC_RATE_HZ;
  if (capacity > s_capacity) {
    heap_caps_free(s_samples);
    s_samples = heap_caps_malloc(capacity * sizeof(float),
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_capacity = s_samples ? capacity : 0U;
    if (!s_samples) return ESP_ERR_NO_MEM;
  }
  if (!s_stamp_count) {
    s_stamp_count = heap_caps_malloc(MIC_MAX_STAMPS * sizeof(uint32_t),
                                     MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_stamp_us = heap_caps_malloc(MIC_MAX_STAMPS * sizeof(int64_t),
                                  MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_stamp_count || !s_stamp_us) return ESP_ERR_NO_MEM;
  }
  s_count = 0;
  s_stamps = 0;
  s_dma_samples = 0;
  s_overflows = 0;
  s_line_valid = false;

  i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
  chan.dma_desc_num = MIC_DMA_DESC;
  chan.dma_frame_num = MIC_DMA_FRAMES;
  esp_err_t err = i2s_new_channel(&chan, NULL, &s_rx);
  if (err != ESP_OK) return err;
  i2s_std_config_t std = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(GW_MIC_RATE_HZ),
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                      I2S_SLOT_MODE_MONO),
      .gpio_cfg = {
          .mclk = I2S_GPIO_UNUSED,
          .bclk = CONFIG_AIRPLAY_SURROUND_CAL_MIC_SCK_GPIO,
          .ws = CONFIG_AIRPLAY_SURROUND_CAL_MIC_WS_GPIO,
          .dout = I2S_GPIO_UNUSED,
          .din = CONFIG_AIRPLAY_SURROUND_CAL_MIC_SD_GPIO,
      },
  };
  std.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT; /* L/R pin to GND */
  const i2s_event_callbacks_t cbs = {.on_recv = on_recv,
                                     .on_recv_q_ovf = on_recv_q_ovf};
  err = i2s_channel_init_std_mode(s_rx, &std);
  if (err == ESP_OK) err = i2s_channel_register_event_callback(s_rx, &cbs, NULL);
  if (err == ESP_OK) err = i2s_channel_enable(s_rx);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "I2S microphone start failed: %s", esp_err_to_name(err));
    i2s_del_channel(s_rx);
    s_rx = NULL;
    return err;
  }
  s_running = true;
  if (xTaskCreatePinnedToCore(reader_task, "gw_mic", MIC_TASK_STACK, NULL,
                              MIC_TASK_PRIORITY, &s_task, 1) != pdPASS) {
    s_running = false;
    i2s_channel_disable(s_rx);
    i2s_del_channel(s_rx);
    s_rx = NULL;
    return ESP_ERR_NO_MEM;
  }
  return ESP_OK;
}

void gw_mic_stop(void) {
  if (!s_rx) return;
  s_running = false;
  for (int i = 0; s_task && i < 50; ++i) vTaskDelay(pdMS_TO_TICKS(10));
  i2s_channel_disable(s_rx);
  i2s_del_channel(s_rx);
  s_rx = NULL;
  fit_line();
  if (s_overflows) {
    ESP_LOGW(TAG, "recording lost %u DMA buffers (reader too slow): no "
                  "usable timing", (unsigned)s_overflows);
    s_line_valid = false;
  }
  if (s_line_valid) {
    ESP_LOGI(TAG, "recorded %u samples, %u DMA stamps, %.3f us/sample",
             (unsigned)s_count, (unsigned)s_stamps, s_b_us);
  } else {
    ESP_LOGW(TAG, "recorded %u samples but no usable timing", (unsigned)s_count);
  }
}

const float *gw_mic_samples(uint32_t *count) {
  if (count) *count = s_count;
  return s_samples;
}

bool gw_mic_sample_time_ns(double index, int64_t *local_ns) {
  if (!s_line_valid) return false;
  *local_ns = (int64_t)((s_a_us + s_b_us * (index + 1.0)) * 1000.0);
  return true;
}

bool gw_mic_sample_at(int64_t local_ns, double *index) {
  if (!s_line_valid) return false;
  *index = ((double)local_ns / 1000.0 - s_a_us) / s_b_us - 1.0;
  return true;
}

#else

esp_err_t gw_mic_start(uint32_t max_seconds) {
  (void)max_seconds;
  return ESP_ERR_NOT_SUPPORTED;
}
void gw_mic_stop(void) {}
const float *gw_mic_samples(uint32_t *count) {
  if (count) *count = 0;
  return NULL;
}
bool gw_mic_sample_time_ns(double index, int64_t *local_ns) {
  (void)index;
  (void)local_ns;
  return false;
}
bool gw_mic_sample_at(int64_t local_ns, double *index) {
  (void)local_ns;
  (void)index;
  return false;
}

#endif
