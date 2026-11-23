#include "gw_local_volume.h"

#include <math.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "gw_radio.h"
#include "sdkconfig.h"
#if CONFIG_GATEWAY_USB_PLAYER
#include "gw_player.h"
#endif
#include "nvs.h"
#include "rtsp_tables.h"
#include "sound_engine.h"

#define VOLUME_UNITY 32768
#define VOLUME_NS "radio"
#define VOLUME_KEY "vol_q15"

static const char *TAG = "gw_local_volume";
static StaticSemaphore_t s_lock_buffer;
static SemaphoreHandle_t s_lock;
static int32_t s_q15;
static int32_t s_saved_q15 = -1;

int32_t gw_local_volume_from_db(int db) {
  if (db < -60) db = -60;
  if (db > 0) db = 0;
  return (int32_t)lroundf(VOLUME_UNITY * powf(10.0f, db / 20.0f));
}

int gw_local_volume_db(int32_t q15) {
  return q15 <= gw_local_volume_from_db(-60)
             ? -60 : (int)lround(20.0 * log10((double)q15 / VOLUME_UNITY));
}

int32_t gw_local_volume_from_percent(double percent) {
  return rtsp_volume_to_q15(rtsp_fraction_to_volume(percent / 100.0));
}

int gw_local_volume_percent(int32_t q15) {
  return (int)lround(100.0 * sqrt((double)q15 / VOLUME_UNITY));
}

void gw_local_volume_init(void) {
  s_lock = xSemaphoreCreateMutexStatic(&s_lock_buffer);
  s_q15 = gw_local_volume_from_db(-30);
  s_saved_q15 = -1;

  /* Use the old radio setting until a shared level has been saved. */
  nvs_handle_t handle;
  if (nvs_open(VOLUME_NS, NVS_READONLY, &handle) != ESP_OK) return;
  int32_t q15;
  int8_t legacy_db;
  if (nvs_get_i32(handle, VOLUME_KEY, &q15) == ESP_OK && q15 >= 0 && q15 <= VOLUME_UNITY) {
    s_q15 = s_saved_q15 = q15;
  } else if (nvs_get_i8(handle, "vol", &legacy_db) == ESP_OK) {
    s_q15 = gw_local_volume_from_db(legacy_db);
  }
  nvs_close(handle);
}

static bool local_active(void) {
#if CONFIG_GATEWAY_USB_PLAYER
  if (gw_player_active()) return true;
#endif
  return gw_radio_active();
}

static esp_err_t persist_locked(int32_t q15) {
  if (q15 == s_saved_q15) return ESP_OK;
  nvs_handle_t handle;
  esp_err_t err = nvs_open(VOLUME_NS, NVS_READWRITE, &handle);
  if (err == ESP_OK) {
    err = nvs_set_i32(handle, VOLUME_KEY, q15);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
  }
  if (err == ESP_OK) {
    s_saved_q15 = q15;
  } else {
    ESP_LOGW(TAG, "Volume not saved: %s", esp_err_to_name(err));
  }
  return err;
}

int32_t gw_local_volume_get(void) {
  xSemaphoreTake(s_lock, portMAX_DELAY);
  const int32_t q15 = s_q15;
  xSemaphoreGive(s_lock);
  return q15;
}

int32_t gw_local_volume_apply(void) {
  xSemaphoreTake(s_lock, portMAX_DELAY);
  const int32_t q15 = s_q15;
  sound_engine_set_volume_q15(q15);
  xSemaphoreGive(s_lock);
  return q15;
}

esp_err_t gw_local_volume_set(int32_t q15, bool save) {
  xSemaphoreTake(s_lock, portMAX_DELAY);
  s_q15 = q15;
  if (local_active()) sound_engine_set_volume_q15(q15);
  const esp_err_t err = save ? persist_locked(q15) : ESP_OK;
  xSemaphoreGive(s_lock);
  return err;
}

bool gw_local_volume_step(bool up) {
  xSemaphoreTake(s_lock, portMAX_DELAY);
  const bool active = local_active();
  if (active) {
    const double fraction = sqrt((double)s_q15 / VOLUME_UNITY);
    const double volume = rtsp_fraction_to_volume(fraction);
    const double stepped = rtsp_volume_step(volume, up);
    s_q15 = rtsp_volume_to_q15(rtsp_fraction_to_volume(stepped));
    sound_engine_set_volume_q15(s_q15);
    (void)persist_locked(s_q15);
  }
  xSemaphoreGive(s_lock);
  return active;
}
