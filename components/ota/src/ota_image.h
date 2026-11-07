/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Checks of the start of an uploaded firmware image. No ESP-IDF headers.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Layout of the start of an application image (ESP-IDF app image format):
 * esp_image_header_t (24 bytes), the first esp_image_segment_header_t
 * (8 bytes), then esp_app_desc_t (256 bytes) at the start of the first
 * segment. src/ota.c checks these numbers against the ESP-IDF headers at
 * compile time. */
#define OTA_IMAGE_HEADER_LEN       24
#define OTA_IMAGE_SEGMENT_HDR_LEN  8
#define OTA_IMAGE_APP_DESC_LEN     256
#define OTA_IMAGE_HEAD_LEN \
    (OTA_IMAGE_HEADER_LEN + OTA_IMAGE_SEGMENT_HDR_LEN + OTA_IMAGE_APP_DESC_LEN)

#define OTA_IMAGE_MAGIC            0xE9u
#define OTA_IMAGE_CHIP_ID_OFFSET   12      /* uint16_t, little endian */
#define OTA_IMAGE_CHIP_ID_ESP32S3  0x0009u
#define OTA_IMAGE_HASH_APPENDED_OFFSET 23  /* uint8_t, must be 1: SHA-256 appended */

#define OTA_IMAGE_APP_DESC_OFFSET  (OTA_IMAGE_HEADER_LEN + OTA_IMAGE_SEGMENT_HDR_LEN)
#define OTA_IMAGE_APP_DESC_MAGIC   0xABCD5432u
/* Offsets inside esp_app_desc_t. */
#define OTA_IMAGE_DESC_VERSION_OFFSET  16
#define OTA_IMAGE_DESC_PROJECT_OFFSET  48
#define OTA_IMAGE_DESC_STR_LEN         32

typedef enum {
    OTA_IMAGE_OK = 0,
    OTA_IMAGE_BAD_SIZE,    /* too long for the partition, or too short */
    OTA_IMAGE_INVALID,     /* not an ESP32-S3 application image, or hash_appended != 1 */
    OTA_IMAGE_OTHER_KIND,  /* another kind of firmware than the running one, or no kind */
} ota_image_result_t;

typedef struct {
    /* NUL-terminated, non-printable bytes replaced by '?'; for logs only. */
    char project[OTA_IMAGE_DESC_STR_LEN + 1];
    char version[OTA_IMAGE_DESC_STR_LEN + 1];
} ota_image_info_t;

/* Size known from Content-Length, before any byte is read. */
ota_image_result_t ota_image_check_size(size_t total_len, size_t partition_size);

/* Checks the first head_len bytes of the image (at least OTA_IMAGE_HEAD_LEN
 * are needed). running_project is the project_name of the running firmware,
 * at most running_len bytes, not necessarily NUL-terminated. info may be
 * NULL; it is filled when the result is OTA_IMAGE_OK or OTA_IMAGE_OTHER_KIND. */
ota_image_result_t ota_image_check_head(const uint8_t *head, size_t head_len,
                                        const char *running_project, size_t running_len,
                                        ota_image_info_t *info);

/* Length of the kind (the part before the first '_') of a project name of at
 * most max_len bytes, or -1 when there is no '_' or the kind is empty. */
int ota_image_kind_len(const char *project, size_t max_len);

/* Progress in steps of 10 %. *last_logged starts at 0. Returns the
 * percentage to log now (10, 20, ... 90), or -1 if nothing to log. 100 % is
 * never returned: the end line takes its place. */
int ota_image_progress(size_t done, size_t total, int *last_logged);

const char *ota_image_result_str(ota_image_result_t r);

#ifdef __cplusplus
}
#endif
