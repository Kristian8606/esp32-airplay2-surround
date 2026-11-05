/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Firmware update from the body of POST /api/ota/update. See README.md.
 */
#include "ota.h"

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_heap_caps.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_timer.h"

#include "ota_image.h"

static const char *TAG = "ota";

#define OTA_CHUNK_LEN        4096
#define OTA_IDLE_TIMEOUT_US  (10 * 1000 * 1000LL)

/* Set while an update by this module runs. A 32-bit atomic: lock-free on
 * the ESP32-S3. */
static atomic_int s_busy;

/* The host-side checks in ota_image.c hard-code the image layout. */
_Static_assert(sizeof(esp_image_header_t) == OTA_IMAGE_HEADER_LEN, "image header size");
_Static_assert(sizeof(esp_image_segment_header_t) == OTA_IMAGE_SEGMENT_HDR_LEN, "segment header size");
_Static_assert(sizeof(esp_app_desc_t) == OTA_IMAGE_APP_DESC_LEN, "app desc size");
_Static_assert(offsetof(esp_image_header_t, chip_id) == OTA_IMAGE_CHIP_ID_OFFSET, "chip_id offset");
_Static_assert(offsetof(esp_image_header_t, hash_appended) == OTA_IMAGE_HASH_APPENDED_OFFSET,
               "hash_appended offset");
_Static_assert(sizeof(esp_chip_id_t) == 2, "chip_id size");
_Static_assert(ESP_IMAGE_HEADER_MAGIC == OTA_IMAGE_MAGIC, "image magic");
_Static_assert(ESP_CHIP_ID_ESP32S3 == OTA_IMAGE_CHIP_ID_ESP32S3, "ESP32-S3 chip id");
_Static_assert(ESP_APP_DESC_MAGIC_WORD == OTA_IMAGE_APP_DESC_MAGIC, "app desc magic");
_Static_assert(offsetof(esp_app_desc_t, version) == OTA_IMAGE_DESC_VERSION_OFFSET, "version offset");
_Static_assert(offsetof(esp_app_desc_t, project_name) == OTA_IMAGE_DESC_PROJECT_OFFSET, "project offset");
_Static_assert(sizeof(((esp_app_desc_t *)0)->project_name) == OTA_IMAGE_DESC_STR_LEN, "project size");
_Static_assert(sizeof(((esp_app_desc_t *)0)->version) == OTA_IMAGE_DESC_STR_LEN, "version size");
_Static_assert(OTA_IMAGE_HEAD_LEN <= OTA_CHUNK_LEN, "head fits the buffer");

/* Reads into buf[*fill..cap) until at least min_fill bytes are there.
 * A read timeout is retried until OTA_IDLE_TIMEOUT_US pass without a byte. */
static esp_err_t recv_until(httpd_req_t *req, uint8_t *buf, size_t *fill, size_t min_fill,
                            size_t cap, size_t *received, int64_t *last_rx_us, const char **why)
{
    while (*fill < min_fill) {
        size_t want = cap - *fill;
        int n = httpd_req_recv(req, (char *)buf + *fill, want);
        if (n > 0 && (size_t)n <= want) {
            *fill += (size_t)n;
            *received += (size_t)n;
            *last_rx_us = esp_timer_get_time();
        } else if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            if (esp_timer_get_time() - *last_rx_us >= OTA_IDLE_TIMEOUT_US) {
                *why = "no data for 10 s";
                return ESP_ERR_TIMEOUT;
            }
        } else {
            *why = (n == 0) ? "connection closed" : "receive error";
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

static esp_err_t image_result_to_err(ota_image_result_t r)
{
    switch (r) {
    case OTA_IMAGE_OK:         return ESP_OK;
    case OTA_IMAGE_BAD_SIZE:   return ESP_ERR_INVALID_SIZE;
    case OTA_IMAGE_INVALID:    return ESP_ERR_IMAGE_INVALID;
    case OTA_IMAGE_OTHER_KIND: return ESP_ERR_NOT_SUPPORTED;
    }
    return ESP_FAIL;
}

/* The board must never point at the partition about to be overwritten: if
 * an earlier update set it to boot next, point back at the running one. */
static esp_err_t boot_back_to_running(const esp_partition_t *running, bool *changed)
{
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    *changed = false;
    if (boot != NULL && boot->address == running->address) {
        return ESP_OK;
    }
    *changed = true;
    return esp_ota_set_boot_partition(running);
}

static esp_err_t ota_run(httpd_req_t *req)
{
    if (req == NULL || req->content_len == 0) {
        ESP_LOGE(TAG, "update refused: empty request (%s)", esp_err_to_name(ESP_ERR_INVALID_ARG));
        return ESP_ERR_INVALID_ARG;
    }
    const size_t total = req->content_len;

    const esp_partition_t *running_part = esp_ota_get_running_partition();
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (part == NULL || running_part == NULL) {
        ESP_LOGE(TAG, "update refused: no update or running partition (%s)", esp_err_to_name(ESP_ERR_NOT_FOUND));
        return ESP_ERR_NOT_FOUND;
    }
    if (ota_image_check_size(total, part->size) != OTA_IMAGE_OK) {
        ESP_LOGE(TAG, "update refused: size %u not in [%u, %u] (%s)", (unsigned)total,
                 (unsigned)OTA_IMAGE_HEAD_LEN, (unsigned)part->size, esp_err_to_name(ESP_ERR_INVALID_SIZE));
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t *buf = heap_caps_malloc(OTA_CHUNK_LEN, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        ESP_LOGE(TAG, "update failed: no PSRAM for the buffer (%s)", esp_err_to_name(ESP_ERR_NO_MEM));
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = ESP_OK;
    const char *why = NULL;
    esp_ota_handle_t handle = 0;
    bool open = false;
    size_t received = 0;
    size_t fill = 0;
    int last_logged = 0;
    int64_t last_rx_us = esp_timer_get_time();
    ota_image_info_t info = { .project = "", .version = "" };
    const esp_app_desc_t *running = esp_app_get_description();
    ota_image_result_t r;
    bool boot_changed = false;

    /* Nothing goes to flash before the start of the image is checked. */
    size_t cap = total < OTA_CHUNK_LEN ? total : OTA_CHUNK_LEN;
    err = recv_until(req, buf, &fill, OTA_IMAGE_HEAD_LEN, cap, &received, &last_rx_us, &why);
    if (err != ESP_OK) {
        goto fail;
    }

    r = ota_image_check_head(buf, fill, running->project_name,
                             sizeof(running->project_name), &info);
    if (r != OTA_IMAGE_OK) {
        err = image_result_to_err(r);
        why = ota_image_result_str(r);
        goto fail;
    }

    /* Before esp_ota_begin, which may already erase. */
    err = boot_back_to_running(running_part, &boot_changed);
    if (err != ESP_OK) {
        why = "esp_ota_set_boot_partition (back to the running firmware)";
        goto fail;
    }

    ESP_LOGI(TAG, "update: %u bytes to %s at 0x%lx, image %s version %s%s%s", (unsigned)total, part->label,
             (unsigned long)part->address, info.project, info.version,
             boot_changed ? ", boot set back to " : "", boot_changed ? running_part->label : "");

    err = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &handle);
    if (err != ESP_OK) {
        why = "esp_ota_begin";
        goto fail;
    }
    open = true;

    for (;;) {
        err = esp_ota_write(handle, buf, fill);
        if (err != ESP_OK) {
            why = "esp_ota_write";
            goto fail;
        }
        int pct = ota_image_progress(received, total, &last_logged);
        if (pct > 0) {
            ESP_LOGI(TAG, "update: %d %%", pct);
        }
        if (received == total) {
            break;
        }
        fill = 0;
        cap = total - received < OTA_CHUNK_LEN ? total - received : OTA_CHUNK_LEN;
        err = recv_until(req, buf, &fill, cap, cap, &received, &last_rx_us, &why);
        if (err != ESP_OK) {
            goto fail;
        }
    }

    /* esp_ota_end validates the whole image and frees the handle either way. */
    open = false;
    err = esp_ota_end(handle);
    if (err != ESP_OK) {
        why = "esp_ota_end (image validation)";
        goto fail;
    }
    err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK) {
        why = "esp_ota_set_boot_partition";
        goto fail;
    }

    heap_caps_free(buf);
    ESP_LOGI(TAG, "update done: %s boots next", part->label);
    return ESP_OK;

fail:
    if (open) {
        esp_ota_abort(handle);
    }
    heap_caps_free(buf);
    ESP_LOGE(TAG, "update failed after %u of %u bytes, image '%s': %s (%s)", (unsigned)received,
             (unsigned)total, info.project, why ? why : "?", esp_err_to_name(err));
    return err;
}

esp_err_t ota_start_from_http(httpd_req_t *req)
{
    if (atomic_exchange(&s_busy, 1) != 0) {
        ESP_LOGE(TAG, "update refused: an update is already running (%s)",
                 esp_err_to_name(ESP_ERR_INVALID_STATE));
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = ota_run(req);
    atomic_store(&s_busy, 0);
    return err;
}
