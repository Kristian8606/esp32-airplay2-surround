#include "led.h"
#include "sdkconfig.h"

#include <math.h>
#include <string.h>

#include "airplay_log.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "rtsp_events.h"

static const char *TAG = "rgb_vu";

#if CONFIG_ENABLE_RGB_AUDIO_LED
#include "led_strip.h"

#ifndef CONFIG_RGB_AUDIO_LED_GPIO
#define CONFIG_RGB_AUDIO_LED_GPIO 38
#endif
#ifndef CONFIG_RGB_AUDIO_LED_BRIGHTNESS
#define CONFIG_RGB_AUDIO_LED_BRIGHTNESS 96
#endif
#ifndef CONFIG_RGB_AUDIO_LED_UPDATE_HZ
#define CONFIG_RGB_AUDIO_LED_UPDATE_HZ 30
#endif

#define SILENCE_THRESH 200.0f
#define VU_FULL_SCALE 16000.0f
#define UPDATE_INTERVAL_US (1000000LL / CONFIG_RGB_AUDIO_LED_UPDATE_HZ)
#define LED_PCM_MAX_FRAMES 256U
#define LED_TASK_STACK 4096U
#define LED_TASK_PRIO 1U
/* Multichannel: FDK keeps Core0 ~90% busy at a higher priority; Core1 has
 * idle time for the VU animation. */
#if defined(CONFIG_AIRPLAY_MULTICHANNEL) && CONFIG_AIRPLAY_MULTICHANNEL
#define LED_TASK_CORE 1
#else
#define LED_TASK_CORE 0
#endif

typedef enum {
  LED_STATE_STANDBY,
  LED_STATE_PAUSED,
  LED_STATE_PLAYING,
} led_state_t;

static led_strip_handle_t s_strip;
static TaskHandle_t s_led_task;
static volatile uint32_t s_requested_state = LED_STATE_STANDBY;
static const uint32_t s_brightness = CONFIG_RGB_AUDIO_LED_BRIGHTNESS;
static int64_t s_last_capture_us;

/* Audio -> LED single-latest-frame mailbox. The high-priority playout task
 * only copies at most one 256-frame stereo block at the configured LED rate
 * and wakes the low-priority LED task. All RMS/HSV/RMT work happens there. */
static int16_t s_pcm_mailbox[LED_PCM_MAX_FRAMES * 2U];
static volatile uint32_t s_pcm_frames;
static volatile uint32_t s_pcm_seq;

static inline void led_wake(void) {
  TaskHandle_t task = __atomic_load_n(&s_led_task, __ATOMIC_ACQUIRE);
  if (task) xTaskNotifyGive(task);
}

static uint8_t scale_bright(uint8_t v) {
  return (uint8_t)(((uint16_t)v * s_brightness) / 255U);
}

/* These helpers are called only by led_task after init, making the RMT/strip
 * handle single-owner at runtime. */
static void rgb_refresh_color(uint8_t r, uint8_t g, uint8_t b) {
  if (!s_strip) return;
  led_strip_set_pixel(s_strip, 0, r, g, b);
  esp_err_t err = led_strip_refresh(s_strip);
  if (err != ESP_OK) {
    AP_LOGW(TAG, "refresh failed: %s", esp_err_to_name(err));
  }
}

static void rgb_clear(void) {
  if (!s_strip) return;
  led_strip_clear(s_strip);
  esp_err_t err = led_strip_refresh(s_strip);
  if (err != ESP_OK) {
    AP_LOGW(TAG, "clear refresh failed: %s", esp_err_to_name(err));
  }
}

static void render_state(led_state_t state) {
  switch (state) {
    case LED_STATE_PLAYING:
#if defined(CONFIG_RGB_AUDIO_LED_PLAYING_OFF)
      rgb_clear();
#elif defined(CONFIG_RGB_AUDIO_LED_PLAYING_STEADY)
      rgb_refresh_color(0, scale_bright(0x60), scale_bright(0x10));
#else
      /* VU mode is rendered from the latest PCM mailbox. */
#endif
      break;

    case LED_STATE_PAUSED:
#if defined(CONFIG_RGB_AUDIO_LED_PAUSED_OFF)
      rgb_clear();
#else
      rgb_refresh_color(0, 0, scale_bright(0x60));
#endif
      break;

    case LED_STATE_STANDBY:
#if defined(CONFIG_RGB_AUDIO_LED_STANDBY_STEADY)
      rgb_refresh_color(0, scale_bright(0x28), 0);
#else
      rgb_clear();
#endif
      break;
  }
}

static bool pcm_mailbox_snapshot(int16_t *dst, uint32_t *frames_out) {
  if (!dst || !frames_out) return false;
  for (int attempt = 0; attempt < 3; ++attempt) {
    const uint32_t seq1 = __atomic_load_n(&s_pcm_seq, __ATOMIC_ACQUIRE);
    if (seq1 & 1U) {
      taskYIELD();
      continue;
    }
    uint32_t frames = __atomic_load_n(&s_pcm_frames, __ATOMIC_RELAXED);
    if (frames > LED_PCM_MAX_FRAMES) frames = LED_PCM_MAX_FRAMES;
    if (frames == 0) return false;
    memcpy(dst, s_pcm_mailbox, frames * 2U * sizeof(int16_t));
    const uint32_t seq2 = __atomic_load_n(&s_pcm_seq, __ATOMIC_ACQUIRE);
    if (seq1 == seq2 && !(seq2 & 1U)) {
      *frames_out = frames;
      return true;
    }
  }
  return false;
}

static void render_vu(const int16_t *pcm, size_t stereo_frames) {
#if defined(CONFIG_RGB_AUDIO_LED_PLAYING_VU)
  if (!pcm || stereo_frames == 0 || !s_strip) return;

  const size_t total_samples = stereo_frames * 2U;
  uint64_t sum_sq = 0;
  for (size_t i = 0; i < total_samples; ++i) {
    const int32_t sample = pcm[i];
    sum_sq += (uint64_t)((int64_t)sample * (int64_t)sample);
  }
  const float rms = sqrtf((float)sum_sq / (float)total_samples);

  uint64_t diff_sum = 0;
  size_t diff_count = 0;
  for (size_t i = 2; i < total_samples; i += 2) {
    const int32_t d = (int32_t)pcm[i] - (int32_t)pcm[i - 2];
    diff_sum += (uint64_t)(d < 0 ? -(int64_t)d : (int64_t)d);
    ++diff_count;
  }
  const float high_energy =
      diff_count ? (float)diff_sum / (float)diff_count : 0.0f;

  float bass_ratio = 0.0f;
  if (rms > SILENCE_THRESH) {
    bass_ratio = 1.0f - high_energy / (rms * 2.0f + 1.0f);
    if (bass_ratio < 0.0f) bass_ratio = 0.0f;
    if (bass_ratio > 1.0f) bass_ratio = 1.0f;
  }

  float norm = 0.0f;
  if (rms >= SILENCE_THRESH) {
    norm = (rms - SILENCE_THRESH) / (VU_FULL_SCALE - SILENCE_THRESH);
    if (norm > 1.0f) norm = 1.0f;
  }

  const uint32_t brightness =
      __atomic_load_n(&s_brightness, __ATOMIC_ACQUIRE);
  if (norm <= 0.0f || brightness == 0) {
    rgb_clear();
    return;
  }

  uint8_t val = (uint8_t)(norm * (float)brightness);
  if (val == 0) val = 1;
  uint16_t hue = (uint16_t)(170.0f * (1.0f - norm));
  if (bass_ratio > 0.30f) {
    hue += (uint16_t)(bass_ratio * 60.0f);
    if (hue > 255U) hue = 255U;
  }
  uint8_t sat = 255U;
  if (norm > 0.85f) {
    sat = (uint8_t)(255.0f - ((norm - 0.85f) / 0.15f) * 80.0f);
  }

  led_strip_set_pixel_hsv(s_strip, 0, hue, sat, val);
  esp_err_t err = led_strip_refresh(s_strip);
  if (err != ESP_OK) {
    AP_LOGW(TAG, "VU refresh failed: %s", esp_err_to_name(err));
  }
#else
  (void)pcm;
  (void)stereo_frames;
#endif
}

static void led_task(void *arg) {
  (void)arg;
  int16_t pcm[LED_PCM_MAX_FRAMES * 2U];
  led_state_t last_state = LED_STATE_STANDBY;
  bool have_rendered_state = false;

  while (1) {
    (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    const led_state_t desired =
        (led_state_t)__atomic_load_n(&s_requested_state, __ATOMIC_ACQUIRE);

    if (!have_rendered_state || desired != last_state) {
      if (have_rendered_state) {
        AP_LOGI(TAG, "state %d -> %d", (int)last_state, (int)desired);
      }
      last_state = desired;
      have_rendered_state = true;
      render_state(desired);
    }

#if defined(CONFIG_RGB_AUDIO_LED_PLAYING_VU)
    if (desired == LED_STATE_PLAYING) {
      uint32_t frames = 0;
      if (pcm_mailbox_snapshot(pcm, &frames)) {
        render_vu(pcm, frames);
      }
    }
#endif
  }
}

static void on_rtsp_event(rtsp_event_t event, void *user_data) {
  (void)user_data;
  led_state_t state;
  switch (event) {
    case RTSP_EVENT_CLIENT_CONNECTED:
      state = LED_STATE_PAUSED;
      break;
    case RTSP_EVENT_PLAYING:
      state = LED_STATE_PLAYING;
      break;
    case RTSP_EVENT_PAUSED:
      state = LED_STATE_PAUSED;
      break;
    case RTSP_EVENT_DISCONNECTED:
      state = LED_STATE_STANDBY;
      break;
    default:
      return;
  }
  __atomic_store_n(&s_requested_state, (uint32_t)state, __ATOMIC_RELEASE);
  led_wake();
}

void led_audio_feed(const audio_out_sample_t *pcm, size_t stereo_frames) {
#if defined(CONFIG_RGB_AUDIO_LED_PLAYING_VU)
  if (!pcm || stereo_frames == 0 || !s_strip || !s_led_task ||
      __atomic_load_n(&s_requested_state, __ATOMIC_ACQUIRE) !=
          LED_STATE_PLAYING) {
    return;
  }

  const int64_t now = esp_timer_get_time();
  if (now - s_last_capture_us < UPDATE_INTERVAL_US) return;
  s_last_capture_us = now;

  if (stereo_frames > LED_PCM_MAX_FRAMES) stereo_frames = LED_PCM_MAX_FRAMES;
  uint32_t seq = __atomic_load_n(&s_pcm_seq, __ATOMIC_RELAXED);
  if (seq & 1U) ++seq;
  __atomic_store_n(&s_pcm_seq, seq + 1U, __ATOMIC_RELEASE);
  for (size_t i = 0; i < stereo_frames * 2U; ++i)
    s_pcm_mailbox[i] = audio_out_to_pcm16(pcm[i]);
  __atomic_store_n(&s_pcm_frames, (uint32_t)stereo_frames, __ATOMIC_RELAXED);
  __atomic_store_n(&s_pcm_seq, seq + 2U, __ATOMIC_RELEASE);
  led_wake();
#else
  (void)pcm;
  (void)stereo_frames;
#endif
}

void led_init(void) {
  led_strip_config_t strip_cfg = {
      .strip_gpio_num = CONFIG_RGB_AUDIO_LED_GPIO,
      .max_leds = 1,
      .led_model = LED_MODEL_WS2812,
      .flags.invert_out = false,
  };
  led_strip_rmt_config_t rmt_cfg = {
      .clk_src = RMT_CLK_SRC_DEFAULT,
      .resolution_hz = 10 * 1000 * 1000,
      .flags.with_dma = false,
  };

  esp_err_t err = led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "WS2812 init failed GPIO=%d: %s",
             CONFIG_RGB_AUDIO_LED_GPIO, esp_err_to_name(err));
    s_strip = NULL;
    return;
  }

  /* Stack in PSRAM: internal RAM is scarce during a session, and this task
   * never exits and never writes flash. */
  BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(
      led_task, "rgb_led", LED_TASK_STACK, NULL, LED_TASK_PRIO, &s_led_task,
      LED_TASK_CORE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (ok != pdPASS) {
    ESP_LOGE(TAG, "LED task creation failed");
    s_led_task = NULL;
    return;
  }

  if (rtsp_events_register(on_rtsp_event, NULL) != 0) {
    ESP_LOGW(TAG, "RTSP LED event listener registration failed");
  }

  ESP_LOGI(TAG,
           "WS2812 audio LED ready GPIO=%d brightness=%u update=%dHz task=core%d/prio%d",
           CONFIG_RGB_AUDIO_LED_GPIO,
           (unsigned)s_brightness,
           CONFIG_RGB_AUDIO_LED_UPDATE_HZ, LED_TASK_CORE, LED_TASK_PRIO);
  led_wake();
}

#else  /* CONFIG_ENABLE_RGB_AUDIO_LED */

void led_init(void) {}
void led_audio_feed(const audio_out_sample_t *pcm, size_t stereo_frames) {
  (void)pcm;
  (void)stereo_frames;
}

#endif
