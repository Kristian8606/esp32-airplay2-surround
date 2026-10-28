#include "ota.h"

#include "esp_app_format.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_ota_ops.h"
#include <string.h>
#include <sys/param.h>
#include "airplay_log.h"

static const char *TAG = "ota";

#define OTA_STREAM_BUF_BYTES 4096U
#define OTA_STREAM_FALLBACK_BUF_BYTES 1024U
#define OTA_PROGRESS_STEP_BYTES (256U * 1024U)
#define OTA_HTTP_IDLE_TIMEOUT_US (15LL * 1000LL * 1000LL)

static void ota_log_memory(const char *where) {
  AP_LOGI(TAG,
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

static esp_err_t ota_validate_header(const uint8_t *image, size_t len) {
  if (!image || len < sizeof(esp_image_header_t)) {
    ESP_LOGE(TAG, "Image header too small (%zu bytes)", len);
    return ESP_ERR_INVALID_SIZE;
  }

  const esp_image_header_t *header = (const esp_image_header_t *)image;
  if (header->magic != ESP_IMAGE_HEADER_MAGIC) {
    ESP_LOGE(TAG, "Bad image magic: 0x%02x (expected 0x%02x)", header->magic,
             ESP_IMAGE_HEADER_MAGIC);
    return ESP_ERR_INVALID_STATE;
  }

  if (header->segment_count == 0 ||
      header->segment_count > ESP_IMAGE_MAX_SEGMENTS) {
    ESP_LOGE(TAG, "Bad segment count: %u", header->segment_count);
    return ESP_ERR_INVALID_STATE;
  }

  return ESP_OK;
}

static uint8_t *ota_alloc_stream_buffer(size_t *out_size) {
  if (!out_size) return NULL;

  uint8_t *buf = heap_caps_malloc(
      OTA_STREAM_BUF_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (buf) {
    *out_size = OTA_STREAM_BUF_BYTES;
    return buf;
  }

  AP_LOGW(TAG, "Cannot allocate %u-byte internal OTA buffer; trying %u bytes",
           (unsigned)OTA_STREAM_BUF_BYTES,
           (unsigned)OTA_STREAM_FALLBACK_BUF_BYTES);
  buf = heap_caps_malloc(OTA_STREAM_FALLBACK_BUF_BYTES, MALLOC_CAP_8BIT);
  if (buf) *out_size = OTA_STREAM_FALLBACK_BUF_BYTES;
  return buf;
}

esp_err_t ota_start_from_http(httpd_req_t *req) {
  if (!req || req->content_len <= 0) return ESP_ERR_INVALID_ARG;

  const size_t fw_size = (size_t)req->content_len;
  const esp_partition_t *ota_partition =
      esp_ota_get_next_update_partition(NULL);
  if (!ota_partition) {
    ESP_LOGE(TAG, "No OTA partition found");
    return ESP_ERR_NOT_FOUND;
  }

  if (fw_size > ota_partition->size) {
    ESP_LOGE(TAG, "Firmware too large: %zu bytes, OTA partition is %zu bytes",
             fw_size, ota_partition->size);
    return ESP_ERR_INVALID_SIZE;
  }

  ota_log_memory("before-stream");

  size_t buf_size = 0;
  uint8_t *buf = ota_alloc_stream_buffer(&buf_size);
  if (!buf) {
    ESP_LOGE(TAG, "Cannot allocate OTA stream buffer");
    return ESP_ERR_NO_MEM;
  }

  /* Read enough bytes to validate the ESP image header before touching flash. */
  size_t first_len = 0;
  size_t remaining = fw_size;
  int64_t idle_deadline = esp_timer_get_time() + OTA_HTTP_IDLE_TIMEOUT_US;
  while (first_len < sizeof(esp_image_header_t) && remaining > 0) {
    size_t want = MIN(remaining, buf_size - first_len);
    int recv_len = httpd_req_recv(req, (char *)buf + first_len, want);
    if (recv_len == HTTPD_SOCK_ERR_TIMEOUT) {
      if (esp_timer_get_time() >= idle_deadline) {
        ESP_LOGE(TAG, "OTA receive timed out before image header");
        heap_caps_free(buf);
        return ESP_ERR_TIMEOUT;
      }
      continue;
    }
    if (recv_len <= 0) {
      ESP_LOGE(TAG, "Receive error before image header: %d", recv_len);
      heap_caps_free(buf);
      return ESP_FAIL;
    }
    first_len += (size_t)recv_len;
    remaining -= (size_t)recv_len;
    idle_deadline = esp_timer_get_time() + OTA_HTTP_IDLE_TIMEOUT_US;
  }

  esp_err_t err = ota_validate_header(buf, first_len);
  if (err != ESP_OK) {
    heap_caps_free(buf);
    return err;
  }

  esp_ota_handle_t ota_handle;
  err = esp_ota_begin(ota_partition, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
    heap_caps_free(buf);
    return err;
  }

  AP_LOGI(TAG,
           "Receiving firmware via sequential streaming (%zu bytes, buffer=%zu bytes)...",
           fw_size, buf_size);

  size_t written = 0;
  if (first_len > 0) {
    err = esp_ota_write(ota_handle, buf, first_len);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "Flash write failed at offset 0: %s", esp_err_to_name(err));
      esp_ota_abort(ota_handle);
      heap_caps_free(buf);
      return err;
    }
    written = first_len;
  }

  size_t next_progress = OTA_PROGRESS_STEP_BYTES;
  idle_deadline = esp_timer_get_time() + OTA_HTTP_IDLE_TIMEOUT_US;
  while (remaining > 0) {
    size_t want = MIN(remaining, buf_size);
    int recv_len = httpd_req_recv(req, (char *)buf, want);

    if (recv_len == HTTPD_SOCK_ERR_TIMEOUT) {
      if (esp_timer_get_time() >= idle_deadline) {
        ESP_LOGE(TAG, "OTA receive timed out after %zu/%zu bytes", written,
                 fw_size);
        esp_ota_abort(ota_handle);
        heap_caps_free(buf);
        return ESP_ERR_TIMEOUT;
      }
      continue;
    }
    if (recv_len <= 0) {
      ESP_LOGE(TAG, "Receive error: %d after %zu/%zu bytes", recv_len,
               written, fw_size);
      esp_ota_abort(ota_handle);
      heap_caps_free(buf);
      return ESP_FAIL;
    }

    err = esp_ota_write(ota_handle, buf, (size_t)recv_len);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "Flash write failed at offset %zu: %s", written,
               esp_err_to_name(err));
      esp_ota_abort(ota_handle);
      heap_caps_free(buf);
      return err;
    }

    written += (size_t)recv_len;
    remaining -= (size_t)recv_len;
    idle_deadline = esp_timer_get_time() + OTA_HTTP_IDLE_TIMEOUT_US;

    if (written >= next_progress) {
      AP_LOGI(TAG, "OTA progress: %zu/%zu bytes", written, fw_size);
      while (next_progress <= written) next_progress += OTA_PROGRESS_STEP_BYTES;
    }
  }

  heap_caps_free(buf);

  err = esp_ota_end(ota_handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Image validation failed: %s", esp_err_to_name(err));
    return err;
  }

  err = esp_ota_set_boot_partition(ota_partition);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to set boot partition: %s", esp_err_to_name(err));
    return err;
  }

  ota_log_memory("after-stream");
  ESP_LOGI(TAG, "OTA update successful (sequential streaming)");
  return ESP_OK;
}
