/* The Gateway's USB host and its drive (phase 4). See gw_usb.h. */

#include "gw_usb.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "gw_player.h"
#include "usb/msc_host.h"
#include "usb/msc_host_vfs.h"
#include "usb/usb_host.h"

static const char *TAG = "gw_usb";

#define USB_TASK_CORE     0
#define USB_LIB_PRIORITY  5
#define USB_MSC_PRIORITY  5
#define USB_TASK_STACK    6144 /* mounting: FatFs with long names */
#define USB_MAX_FILES     4 /* the player's stream and its seeks, a folder listing */

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static gw_usb_status_t s_st = {.state = "starting"};
static QueueHandle_t s_q;
static volatile bool s_ready;

static void set_state(const char *state, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void set_state(const char *state, const char *fmt, ...) {
  taskENTER_CRITICAL(&s_mux);
  s_st.state = state;
  s_st.message[0] = '\0';
  if (fmt) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_st.message, sizeof(s_st.message), fmt, ap);
    va_end(ap);
  }
  taskEXIT_CRITICAL(&s_mux);
}

static void lib_task(void *arg) {
  const usb_host_config_t cfg = {.skip_phy_setup = false, .intr_flags = ESP_INTR_FLAG_LEVEL1};
  const esp_err_t err = usb_host_install(&cfg);
  xTaskNotify((TaskHandle_t)arg, (uint32_t)err, eSetValueWithOverwrite);
  if (err != ESP_OK) vTaskDelete(NULL);
  for (;;) {
    uint32_t flags = 0;
    usb_host_lib_handle_events(portMAX_DELAY, &flags);
    if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) usb_host_device_free_all();
  }
}

static void msc_cb(const msc_host_event_t *e, void *arg) {
  (void)arg;
  xQueueSend(s_q, e, 0);
}

static void drive_task(void *arg) {
  (void)arg;
  msc_host_device_handle_t dev = NULL;
  msc_host_vfs_handle_t vfs = NULL;
  for (;;) {
    msc_host_event_t e;
    if (xQueueReceive(s_q, &e, portMAX_DELAY) != pdTRUE) continue;
    if (e.event == MSC_DEVICE_CONNECTED) {
      if (dev) continue; /* one drive at a time */
      set_state("mounting", NULL);
      if (msc_host_install_device(e.device.address, &dev) != ESP_OK) {
        dev = NULL;
        set_state("error", "The drive did not answer as a USB mass storage device");
        continue;
      }
      msc_host_device_info_t info = {0};
      if (msc_host_get_device_info(dev, &info) == ESP_OK) {
        taskENTER_CRITICAL(&s_mux);
        s_st.vid = info.idVendor;
        s_st.pid = info.idProduct;
        s_st.size_bytes = (uint64_t)info.sector_count * info.sector_size;
        taskEXIT_CRITICAL(&s_mux);
        ESP_LOGI(TAG, "drive %04x:%04x, %.1f GB", info.idVendor, info.idProduct,
                 (double)info.sector_count * info.sector_size / 1e9);
      }
      const esp_vfs_fat_mount_config_t mnt = {.format_if_mount_failed = false,
                                              .max_files = USB_MAX_FILES,
                                              .allocation_unit_size = 0};
      const esp_err_t r = msc_host_vfs_register(dev, GW_USB_MOUNT, &mnt, &vfs);
      if (r != ESP_OK) {
        vfs = NULL;
        ESP_LOGW(TAG, "mount failed: %s", esp_err_to_name(r));
        set_state("not FAT32", "The drive is not FAT32 (exFAT, NTFS, APFS and ext4 are not read)");
        continue;
      }
      uint64_t total = 0, free_b = 0;
      if (esp_vfs_fat_info(GW_USB_MOUNT, &total, &free_b) == ESP_OK) {
        taskENTER_CRITICAL(&s_mux);
        s_st.size_bytes = total;
        s_st.free_bytes = free_b;
        taskEXIT_CRITICAL(&s_mux);
      }
      s_ready = true;
      set_state("ready", NULL);
      ESP_LOGI(TAG, "drive mounted at %s: %.1f GB, %.1f GB free", GW_USB_MOUNT, total / 1e9,
               free_b / 1e9);
    } else if (e.event == MSC_DEVICE_DISCONNECTED) {
      ESP_LOGI(TAG, "drive pulled out");
      s_ready = false;
      /* a file from it may be playing: the player lets go first */
      if (gw_player_active() && gw_player_plays_usb()) gw_player_stop();
      if (vfs) msc_host_vfs_unregister(vfs);
      vfs = NULL;
      if (dev) msc_host_uninstall_device(dev);
      dev = NULL;
      taskENTER_CRITICAL(&s_mux);
      s_st.vid = s_st.pid = 0;
      s_st.size_bytes = s_st.free_bytes = 0;
      taskEXIT_CRITICAL(&s_mux);
      set_state("no drive", NULL);
    }
  }
}

esp_err_t gw_usb_start(void) {
  s_q = xQueueCreate(4, sizeof(msc_host_event_t));
  if (!s_q) return ESP_ERR_NO_MEM;
  if (xTaskCreatePinnedToCore(lib_task, "usb_lib", USB_TASK_STACK, xTaskGetCurrentTaskHandle(),
                              USB_LIB_PRIORITY, NULL, USB_TASK_CORE) != pdPASS)
    return ESP_ERR_NO_MEM;
  uint32_t err = ESP_FAIL;
  xTaskNotifyWait(0, UINT32_MAX, &err, portMAX_DELAY);
  if ((esp_err_t)err != ESP_OK) {
    set_state("error", "USB host did not start: %s", esp_err_to_name((esp_err_t)err));
    return (esp_err_t)err;
  }
  const msc_host_driver_config_t mc = {.create_backround_task = true,
                                       .task_priority = USB_MSC_PRIORITY,
                                       .stack_size = USB_TASK_STACK,
                                       .core_id = USB_TASK_CORE,
                                       .callback = msc_cb};
  esp_err_t r = msc_host_install(&mc);
  if (r != ESP_OK) {
    set_state("error", "USB mass storage did not start: %s", esp_err_to_name(r));
    return r;
  }
  if (xTaskCreatePinnedToCore(drive_task, "usb_drive", USB_TASK_STACK, NULL, USB_MSC_PRIORITY - 1,
                              NULL, USB_TASK_CORE) != pdPASS)
    return ESP_ERR_NO_MEM;
  set_state("no drive", NULL);
  ESP_LOGI(TAG, "USB host ready: a FAT32 drive goes on the native USB port (5 V on VBUS)");
  return ESP_OK;
}

bool gw_usb_ready(void) { return s_ready; }

void gw_usb_status(gw_usb_status_t *out) {
  taskENTER_CRITICAL(&s_mux);
  *out = s_st;
  taskEXIT_CRITICAL(&s_mux);
}
