/* Surround Gateway firmware: the AirPlay receiver the Apple TV plays to. */
#include "audio_receiver.h"
#include "audio_diag.h"
#include "amp_control.h"
#include "gw_fw.h"
#include "gw_speakers.h"
#include "dns_server.h"
#include "hap.h"
#include "log_stream.h"
#include "led.h"
#include "mdns_airplay.h"
#include "nvs_flash.h"
#include "ptp_clock.h"
#include "rtsp_server.h"
#include "playback_buttons.h"
#include "settings.h"
#include "sync_pulse.h"
#include "web_server.h"
#include "wifi.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "main";
#define AP_IP_ADDR 0x0104A8C0

static void log_memory_state(const char *where) {
  ESP_LOGI(TAG,
           "MEM %s internalFree=%u KiB internalLargest=%u KiB internalMin=%u KiB "
           "psramFree=%u KiB psramLargest=%u KiB psramMin=%u KiB",
           where,
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024U),
           (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024U),
           (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024U),
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024U),
           (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024U),
           (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM) / 1024U));
}
static void print_firmware_banner(void) {
  const esp_app_desc_t *app = esp_app_get_description();
  ESP_LOGI(TAG, "============================================================");
  ESP_LOGI(TAG, "FIRMWARE: %s", app ? app->project_name : "airplay-esp32");
  ESP_LOGI(TAG, "VERSION: %s", app ? app->version : "unknown");
  ESP_LOGI(TAG, "BUILD: %s %s", __DATE__, __TIME__);
  ESP_LOGI(TAG, "IDF: %s", app ? app->idf_ver : "unknown");
  ESP_LOGI(TAG, "ROLE: Surround Gateway (no decoder, no I2S output)");
  ESP_LOGI(TAG, "CORE PLAN: Core0=RTSP/PTP; Core1=WiFi/LwIP + buffered audio relay + status/LED/log");
  ESP_LOGI(TAG, "============================================================");
}

void app_main(void) {
  print_firmware_banner();
  log_memory_state("boot");
  ESP_ERROR_CHECK(amp_control_init());

  esp_err_t e = nvs_flash_init();
  if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    e = nvs_flash_init();
  }
  ESP_ERROR_CHECK(e);
  ESP_ERROR_CHECK(settings_init());
  ESP_ERROR_CHECK(log_stream_init());

  const bool provisioning_only = !settings_has_wifi_credentials();

  wifi_init_apsta(NULL, NULL);
  ESP_ERROR_CHECK(web_server_start(80));

  if (provisioning_only || !wifi_wait_connected(30000)) {
    ESP_LOGW(TAG,
             "STA not connected; captive WiFi setup remains available at 192.168.4.1");
    dns_server_start(AP_IP_ADDR);
  } else {
    ESP_LOGI(TAG, "WiFi connected");
  }
  log_memory_state("post-wifi");

  /* Optional diagnostics must exist before PTP/audio init so category-owned
   * startup events are captured by the low-priority worker. This is a no-op
   * when diagnostics are disabled or no category is selected. */
  (void)AUDIO_DIAG_INIT();
  ESP_ERROR_CHECK(ptp_clock_init());
  ESP_ERROR_CHECK(hap_init());
  led_init();

  if (provisioning_only) {
    /* First-time setup needs Wi-Fi scan headroom, not a dormant 6 MiB audio
     * FIFO. Saving Wi-Fi already restarts the ESP, so defer the entire AirPlay
     * runtime until that normal connected boot. */
    ESP_LOGI(TAG,
             "WiFi provisioning mode: audio engine deferred until credentials are saved and device restarts");
    log_memory_state("setup-ready");
  } else {
    ESP_ERROR_CHECK(audio_receiver_init());
    log_memory_state("post-audio-init");
    /* Sync test pulse (off unless enabled in menuconfig). */
    esp_err_t pulse_err = sync_pulse_start();
    if (pulse_err != ESP_OK) {
      ESP_LOGW(TAG, "Sync test pulse unavailable: %s", esp_err_to_name(pulse_err));
    }
    mdns_airplay_init();
    ESP_ERROR_CHECK(rtsp_server_start());
    /* Speaker firmware to hand out; without it speakers update by hand. */
    (void)gw_fw_init();
    /* Without the listener AirPlay still works; speakers just cannot join. */
    esp_err_t gw_err = gw_speakers_start();
    if (gw_err != ESP_OK) {
      ESP_LOGE(TAG, "Speaker control link unavailable: %s", esp_err_to_name(gw_err));
    }
    esp_err_t buttons_err = playback_buttons_init();
    if (buttons_err != ESP_OK) {
      ESP_LOGW(TAG, "Playback buttons unavailable: %s", esp_err_to_name(buttons_err));
    }
  }

  const esp_app_desc_t *app = esp_app_get_description();
  const char *fw_version = app ? app->version : "unknown";
  char ip[32] = {0};
  if (wifi_get_ip_str(ip, sizeof(ip)) == ESP_OK) {
    ESP_LOGI(TAG, "Surround Gateway %s ready; Web UI / WiFi scan / logs / OTA: http://%s/",
             fw_version, ip);
  } else {
    ESP_LOGI(TAG, "Surround Gateway %s ready; setup UI: http://192.168.4.1/",
             fw_version);
  }

  /* All services own their workers. Return so IDF frees the main stack. */
}
