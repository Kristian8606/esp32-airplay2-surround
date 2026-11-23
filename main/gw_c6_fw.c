#include "gw_c6_fw.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_hosted.h"
#include "esp_hosted_ota.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gw_c6_time.h"
#include "nvs.h"

static const char *TAG = "gw_c6_fw";

extern const uint8_t gw_c6_fw_start_sym[] asm("_binary_gw_c6_fw_bin_start");
extern const uint8_t gw_c6_fw_end_sym[] asm("_binary_gw_c6_fw_bin_end");

#define CHUNK 1400               /* as tested in 0.2: ~140 KB/s */
/* After OTAEnd (and OTAActivate) the C6 has its new boot partition set and
 * restarts; ESP-Hosted would then restart the P4 through an abort on the
 * lost link. The Gateway restarts first, cleanly, and resets the C6 again as
 * ESP-Hosted starts. */
#define RESTART_DELAY_MS 300
#define NVS_NS "gw_c6"

static SemaphoreHandle_t s_lock;
static gw_c6_fw_status_t s_st;
static esp_app_desc_t s_img; /* the image's app description */
static uint32_t s_img_crc;
static volatile bool s_busy;

static void set_state(gw_c6_fw_state_t state, uint32_t percent, const char *fmt, ...) {
  char msg[sizeof(s_st.message)];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);
  xSemaphoreTake(s_lock, portMAX_DELAY);
  s_st.state = state;
  s_st.percent = percent;
  strlcpy(s_st.message, msg, sizeof(s_st.message));
  xSemaphoreGive(s_lock);
}

/* The embedded image: an ESP application image whose first segment starts
 * with esp_app_desc_t. */
static bool image_read(void) {
  const size_t len = (size_t)(gw_c6_fw_end_sym - gw_c6_fw_start_sym);
  const size_t off = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);
  if (len < off + sizeof(esp_app_desc_t) || gw_c6_fw_start_sym[0] != ESP_IMAGE_HEADER_MAGIC) {
    return false;
  }
  memcpy(&s_img, gw_c6_fw_start_sym + off, sizeof(s_img));
  if (s_img.magic_word != ESP_APP_DESC_MAGIC_WORD) return false;
  s_img.project_name[sizeof(s_img.project_name) - 1] = '\0';
  s_img.version[sizeof(s_img.version) - 1] = '\0';
  s_img_crc = esp_rom_crc32_le(0, gw_c6_fw_start_sym, len);
  return true;
}

static uint8_t tries_get(void) {
  nvs_handle_t h;
  uint32_t crc = 0;
  uint8_t tries = 0;
  if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
    if (nvs_get_u32(h, "crc", &crc) != ESP_OK || crc != s_img_crc) {
      tries = 0; /* counted for another image */
    } else {
      nvs_get_u8(h, "tries", &tries);
    }
    nvs_close(h);
  }
  return tries;
}

static void tries_set(uint8_t tries) {
  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
  nvs_set_u32(h, "crc", s_img_crc);
  nvs_set_u8(h, "tries", tries);
  nvs_commit(h);
  nvs_close(h);
}

/* Sends the image. On success the C6 restarts into it (and the P4 with it);
 * returns only on an error, or if no restart comes. */
static esp_err_t install(void) {
  const size_t len = (size_t)(gw_c6_fw_end_sym - gw_c6_fw_start_sym);
  esp_hosted_coprocessor_fwver_t v = {0};
  const bool have_ver = esp_hosted_get_coprocessor_fwversion(&v) == ESP_OK;
  ESP_LOGW(TAG, "installing %s %s on the C6 (%u bytes); the Gateway restarts after it",
           s_img.project_name, s_img.version, (unsigned)len);
  set_state(GW_C6_FW_UPDATING, 0, "Installing %s on the C6", s_img.version);
  const int64_t t0 = esp_timer_get_time();
  esp_err_t err = esp_hosted_slave_ota_begin();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "OTA begin: %s", esp_err_to_name(err));
    return err;
  }
  uint32_t last_pct = 0;
  for (size_t off = 0; off < len; off += CHUNK) {
    const size_t n = len - off < CHUNK ? len - off : CHUNK;
    err = esp_hosted_slave_ota_write(gw_c6_fw_start_sym + off, n);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "OTA write at %u: %s", (unsigned)off, esp_err_to_name(err));
      return err;
    }
    const uint32_t pct = (uint32_t)((off + n) * 100U / len);
    if (pct / 10 != last_pct / 10) {
      ESP_LOGI(TAG, "C6 update %u %%", (unsigned)pct);
      set_state(GW_C6_FW_UPDATING, pct, "Installing %s on the C6", s_img.version);
    }
    last_pct = pct;
  }
  err = esp_hosted_slave_ota_end();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "OTA end: %s", esp_err_to_name(err));
    return err;
  }
  /* C6 firmware from ESP-Hosted 2.6 on waits for activate; older ones (the
   * factory 0.0.6) switch partition in OTAEnd and restart by themselves. */
  if (have_ver && (v.major1 > 2 || (v.major1 == 2 && v.minor1 >= 6))) {
    err = esp_hosted_slave_ota_activate();
    if (err != ESP_OK) ESP_LOGW(TAG, "OTA activate: %s", esp_err_to_name(err));
  }
  ESP_LOGI(TAG, "C6 update sent in %.1f s; restarting the Gateway with the C6",
           (double)(esp_timer_get_time() - t0) / 1e6);
  vTaskDelay(pdMS_TO_TICKS(RESTART_DELAY_MS));
  esp_restart();
  return ESP_OK;
}

static void run(bool forced) {
  if (!forced) {
    const uint8_t tries = tries_get();
    if (tries >= GW_C6_FW_MAX_TRIES) {
      ESP_LOGE(TAG, "%u updates of the C6 to %s did not take: trying again at the next "
                    "power-on (or from the web page)",
               (unsigned)tries, s_img.version);
      set_state(GW_C6_FW_GAVE_UP, 0, "%u updates to %s did not take; again at the next power-on",
                (unsigned)tries, s_img.version);
      return;
    }
    tries_set((uint8_t)(tries + 1));
  }
  const esp_err_t err = install();
  set_state(GW_C6_FW_FAILED, 0, "Update failed: %s", esp_err_to_name(err));
}

static void check_task(void *arg) {
  (void)arg;
  const int64_t t0 = esp_timer_get_time();
  gw_c6_time_stats_t st = {0};
  while (esp_timer_get_time() - t0 < (int64_t)GW_C6_FW_HELLO_WAIT_MS * 1000) {
    gw_c6_time_get_stats(&st);
    if (st.hello) break;
    vTaskDelay(pdMS_TO_TICKS(200));
  }
  xSemaphoreTake(s_lock, portMAX_DELAY);
  strlcpy(s_st.c6_version, st.hello ? st.c6_version : "", sizeof(s_st.c6_version));
  xSemaphoreGive(s_lock);
  if (st.hello && strcmp(st.c6_name, s_img.project_name) == 0 &&
      strcmp(st.c6_version, s_img.version) == 0) {
    ESP_LOGI(TAG, "the C6 runs %s %s, as the Gateway carries", st.c6_name, st.c6_version);
    set_state(GW_C6_FW_CURRENT, 0, "Up to date");
    if (tries_get() != 0) tries_set(0);
  } else {
    if (st.hello) {
      ESP_LOGW(TAG, "the C6 runs %s %s, the Gateway carries %s %s", st.c6_name, st.c6_version,
               s_img.project_name, s_img.version);
    } else {
      ESP_LOGW(TAG, "no HELLO from the C6 in %u s: it does not run the Gateway's C6 firmware",
               (unsigned)(GW_C6_FW_HELLO_WAIT_MS / 1000));
    }
    s_busy = true;
    run(false);
    s_busy = false;
  }
  vTaskDelete(NULL);
}

static void manual_task(void *arg) {
  (void)arg;
  run(true);
  s_busy = false;
  vTaskDelete(NULL);
}

esp_err_t gw_c6_fw_start(void) {
  if (s_lock) return ESP_OK;
  s_lock = xSemaphoreCreateMutex();
  if (!s_lock) return ESP_ERR_NO_MEM;
  if (!image_read()) {
    ESP_LOGE(TAG, "the embedded C6 image is not an application image");
    set_state(GW_C6_FW_FAILED, 0, "No C6 image in this build");
    return ESP_ERR_INVALID_STATE;
  }
  strlcpy(s_st.image_version, s_img.version, sizeof(s_st.image_version));
  /* The Gateway's own restarts (after an install) keep the count; switching
   * it on, or its reset pin, starts it again. */
  const esp_reset_reason_t rr = esp_reset_reason();
  if ((rr == ESP_RST_POWERON || rr == ESP_RST_EXT) && tries_get() != 0) {
    ESP_LOGI(TAG, "power-on: the C6 update tries start again");
    tries_set(0);
  }
  set_state(GW_C6_FW_CHECKING, 0, "Checking");
  ESP_LOGI(TAG, "carries %s %s for the C6", s_img.project_name, s_img.version);
  if (xTaskCreate(check_task, "gw_c6_fw", 4096, NULL, 3, NULL) != pdPASS) {
    return ESP_ERR_NO_MEM;
  }
  return ESP_OK;
}

esp_err_t gw_c6_fw_update(void) {
  if (!s_lock || s_busy) return ESP_ERR_INVALID_STATE;
  s_busy = true;
  if (xTaskCreate(manual_task, "gw_c6_fw_up", 4096, NULL, 3, NULL) != pdPASS) {
    s_busy = false;
    return ESP_ERR_NO_MEM;
  }
  return ESP_OK;
}

void gw_c6_fw_get_status(gw_c6_fw_status_t *out) {
  if (!s_lock) {
    memset(out, 0, sizeof(*out));
    return;
  }
  xSemaphoreTake(s_lock, portMAX_DELAY);
  *out = s_st;
  xSemaphoreGive(s_lock);
}
