/* USB player, phase 0: read speed of a USB flash drive on the ESP32-S3's
 * USB OTG port (full speed, 12 Mbit/s). See README.md. */

#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "usb/msc_host.h"
#include "usb/msc_host_vfs.h"
#include "usb/usb_host.h"

static const char *TAG = "BENCH";

#define MNT "/usb"
#define SEQ_LIMIT (8U * 1024U * 1024U)
#define PROGRESS_BYTES (2U * 1024U * 1024U)
#define RANDOM_READS 200U
#define RANDOM_SIZE 4096U

static QueueHandle_t s_q;

static void lib_task(void *arg) {
  (void)arg;
  const usb_host_config_t cfg = {.skip_phy_setup = false,
                                 .intr_flags = ESP_INTR_FLAG_LEVEL1};
  ESP_ERROR_CHECK(usb_host_install(&cfg));
  xTaskNotifyGive((TaskHandle_t)arg);
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

static void seq_read(const char *path, size_t buf_size, off_t file_size) {
  FILE *f = fopen(path, "rb");
  if (!f) {
    ESP_LOGE(TAG, "open %s: errno %d", path, errno);
    return;
  }
  uint8_t *buf = heap_caps_malloc(buf_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
  if (!buf) buf = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
  if (!buf) {
    fclose(f);
    return;
  }
  setvbuf(f, NULL, _IONBF, 0); /* our buffer is the read size */
  const size_t limit = (size_t)file_size < SEQ_LIMIT ? (size_t)file_size : SEQ_LIMIT;
  size_t total = 0;
  const int64_t t0 = esp_timer_get_time();
  size_t next_log = PROGRESS_BYTES;
  while (total < limit) {
    const size_t n = fread(buf, 1, buf_size, f);
    if (n == 0) {
      ESP_LOGW(TAG, "fread stopped at %u B: errno %d, eof %d", (unsigned)total, errno, feof(f));
      break;
    }
    total += n;
    if (total >= next_log) {
      next_log += PROGRESS_BYTES;
      const int64_t el = esp_timer_get_time() - t0;
      ESP_LOGI(TAG, "  ... %u KiB read, %.0f KB/s so far", (unsigned)(total / 1024U),
               el ? total * 1e3 / el : 0.0);
    }
  }
  const int64_t us = esp_timer_get_time() - t0;
  fclose(f);
  free(buf);
  ESP_LOGI(TAG, "sequential, %u KiB reads: %u KiB in %.2f s = %.0f KB/s = %.2f Mbit/s",
           (unsigned)(buf_size / 1024U), (unsigned)(total / 1024U), us / 1e6,
           us ? total * 1e3 / us : 0.0, us ? total * 8.0 / us : 0.0);
}

static void random_read(const char *path, off_t file_size) {
  if (file_size <= (off_t)RANDOM_SIZE) return;
  FILE *f = fopen(path, "rb");
  if (!f) return;
  setvbuf(f, NULL, _IONBF, 0);
  uint8_t *buf = heap_caps_malloc(RANDOM_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
  int64_t sum = 0, max = 0;
  for (uint32_t i = 0; buf && i < RANDOM_READS; ++i) {
    const off_t at = (off_t)(esp_random() % (uint32_t)(file_size - RANDOM_SIZE)) & ~(off_t)511;
    const int64_t t0 = esp_timer_get_time();
    fseek(f, at, SEEK_SET);
    (void)fread(buf, 1, RANDOM_SIZE, f);
    const int64_t dt = esp_timer_get_time() - t0;
    sum += dt;
    if (dt > max) max = dt;
  }
  free(buf);
  fclose(f);
  ESP_LOGI(TAG, "random 4 KiB reads (seek + read): %.1f ms avg, %.1f ms max over %u",
           sum / 1000.0 / RANDOM_READS, max / 1000.0, (unsigned)RANDOM_READS);
}

static void bench_mounted(void) {
  DIR *d = opendir(MNT);
  if (!d) {
    ESP_LOGE(TAG, "opendir: errno %d", errno);
    return;
  }
  char best[300] = "";
  off_t best_size = 0;
  unsigned n = 0;
  struct dirent *de;
  while ((de = readdir(d)) != NULL) {
    char p[300];
    snprintf(p, sizeof(p), MNT "/%s", de->d_name);
    struct stat st;
    if (stat(p, &st) != 0) continue;
    if (n++ < 30)
      ESP_LOGI(TAG, "  %s %s %lld B", S_ISDIR(st.st_mode) ? "dir " : "file", de->d_name,
               (long long)st.st_size);
    if (S_ISREG(st.st_mode) && st.st_size > best_size) {
      best_size = st.st_size;
      strlcpy(best, p, sizeof(best));
    }
  }
  closedir(d);
  if (!best_size) {
    ESP_LOGW(TAG, "no file in the root folder: copy a large one (e.g. a FLAC album)");
    return;
  }
  ESP_LOGI(TAG, "reading %s (%lld B), %u MiB per read size", best, (long long)best_size,
           (unsigned)(SEQ_LIMIT / (1024U * 1024U)));
  static const size_t sizes[] = {4096, 16384, 65536, 262144};
  for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i)
    seq_read(best, sizes[i], best_size);
  random_read(best, best_size);
  ESP_LOGI(TAG, "done: unplug the drive to run again with another one");
}

void app_main(void) {
  s_q = xQueueCreate(4, sizeof(msc_host_event_t));
  xTaskCreatePinnedToCore(lib_task, "usb_lib", 4096, xTaskGetCurrentTaskHandle(), 5, NULL, 0);
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
  const msc_host_driver_config_t mc = {.create_backround_task = true,
                                       .task_priority = 5,
                                       .stack_size = 4096,
                                       .core_id = 0,
                                       .callback = msc_cb};
  ESP_ERROR_CHECK(msc_host_install(&mc));
  ESP_LOGI(TAG, "USB host ready: plug in a FAT32 flash drive (directly or through a hub)");

  msc_host_device_handle_t dev = NULL;
  msc_host_vfs_handle_t vfs = NULL;
  for (;;) {
    msc_host_event_t e;
    if (xQueueReceive(s_q, &e, portMAX_DELAY) != pdTRUE) continue;
    if (e.event == MSC_DEVICE_CONNECTED) {
      ESP_LOGI(TAG, "MSC device at address %u", e.device.address);
      if (msc_host_install_device(e.device.address, &dev) != ESP_OK) continue;
      msc_host_print_descriptors(dev);
      msc_host_device_info_t info;
      if (msc_host_get_device_info(dev, &info) == ESP_OK)
        ESP_LOGI(TAG, "VID %04x PID %04x, %" PRIu32 " sectors of %" PRIu32 " B = %.1f GB",
                 info.idVendor, info.idProduct, info.sector_count, info.sector_size,
                 (double)info.sector_count * info.sector_size / 1e9);
      const esp_vfs_fat_mount_config_t mnt = {.format_if_mount_failed = false,
                                              .max_files = 3,
                                              .allocation_unit_size = 0};
      const esp_err_t r = msc_host_vfs_register(dev, MNT, &mnt, &vfs);
      if (r != ESP_OK) {
        ESP_LOGE(TAG, "mount failed: %s (exFAT and NTFS are not supported by this FatFs)",
                 esp_err_to_name(r));
        continue;
      }
      bench_mounted();
    } else if (e.event == MSC_DEVICE_DISCONNECTED) {
      ESP_LOGI(TAG, "MSC device removed");
      if (vfs) msc_host_vfs_unregister(vfs);
      vfs = NULL;
      if (dev) msc_host_uninstall_device(dev);
      dev = NULL;
    }
  }
}
