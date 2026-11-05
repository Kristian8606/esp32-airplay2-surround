/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ota_image.h"

#include <string.h>

static uint16_t rd_le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rd_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static size_t bounded_len(const char *s, size_t max_len)
{
    size_t n = 0;
    while (n < max_len && s[n] != '\0') {
        n++;
    }
    return n;
}

static void copy_printable(char *dst, const uint8_t *src, size_t max_len)
{
    size_t n = bounded_len((const char *)src, max_len);
    for (size_t i = 0; i < n; i++) {
        dst[i] = (src[i] >= 0x20 && src[i] < 0x7F) ? (char)src[i] : '?';
    }
    dst[n] = '\0';
}

ota_image_result_t ota_image_check_size(size_t total_len, size_t partition_size)
{
    if (total_len > partition_size || total_len < OTA_IMAGE_HEAD_LEN) {
        return OTA_IMAGE_BAD_SIZE;
    }
    return OTA_IMAGE_OK;
}

int ota_image_kind_len(const char *project, size_t max_len)
{
    if (project == NULL) {
        return -1;
    }
    size_t n = bounded_len(project, max_len);
    const char *us = memchr(project, '_', n);
    if (us == NULL || us == project) {
        return -1;
    }
    return (int)(us - project);
}

ota_image_result_t ota_image_check_head(const uint8_t *head, size_t head_len,
                                        const char *running_project, size_t running_len,
                                        ota_image_info_t *info)
{
    if (head == NULL || head_len < OTA_IMAGE_HEAD_LEN) {
        return OTA_IMAGE_BAD_SIZE;
    }
    if (head[0] != OTA_IMAGE_MAGIC) {
        return OTA_IMAGE_INVALID;
    }
    if (rd_le16(head + OTA_IMAGE_CHIP_ID_OFFSET) != OTA_IMAGE_CHIP_ID_ESP32S3) {
        return OTA_IMAGE_INVALID;
    }
    /* 0 is no SHA-256 appended; any value other than 1 is unknown. */
    if (head[OTA_IMAGE_HASH_APPENDED_OFFSET] != 1) {
        return OTA_IMAGE_INVALID;
    }
    const uint8_t *desc = head + OTA_IMAGE_APP_DESC_OFFSET;
    if (rd_le32(desc) != OTA_IMAGE_APP_DESC_MAGIC) {
        return OTA_IMAGE_INVALID;
    }

    const char *project = (const char *)(desc + OTA_IMAGE_DESC_PROJECT_OFFSET);
    if (info != NULL) {
        copy_printable(info->project, desc + OTA_IMAGE_DESC_PROJECT_OFFSET, OTA_IMAGE_DESC_STR_LEN);
        copy_printable(info->version, desc + OTA_IMAGE_DESC_VERSION_OFFSET, OTA_IMAGE_DESC_STR_LEN);
    }

    /* A name without a kind, on either side, never matches. */
    int kind = ota_image_kind_len(project, OTA_IMAGE_DESC_STR_LEN);
    int running_kind = ota_image_kind_len(running_project, running_len);
    if (kind < 0 || running_kind < 0 || kind != running_kind ||
        memcmp(project, running_project, (size_t)kind) != 0) {
        return OTA_IMAGE_OTHER_KIND;
    }
    return OTA_IMAGE_OK;
}

int ota_image_progress(size_t done, size_t total, int *last_logged)
{
    if (total == 0 || last_logged == NULL) {
        return -1;
    }
    if (done > total) {
        done = total;
    }
    /* Tenths done, without overflow for any size_t. */
    size_t tenths = (total <= SIZE_MAX / 10) ? done * 10 / total : done / (total / 10);
    if (tenths > 10) {
        tenths = 10;
    }
    int step = (int)tenths * 10;
    if (step < 100 && step > *last_logged) {
        *last_logged = step;
        return step;
    }
    return -1;
}

const char *ota_image_result_str(ota_image_result_t r)
{
    switch (r) {
    case OTA_IMAGE_OK:         return "ok";
    case OTA_IMAGE_BAD_SIZE:   return "bad size";
    case OTA_IMAGE_INVALID:    return "not an ESP32-S3 application image";
    case OTA_IMAGE_OTHER_KIND: return "another kind of firmware";
    }
    return "?";
}
