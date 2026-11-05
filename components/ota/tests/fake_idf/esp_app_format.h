/* SPDX-License-Identifier: Apache-2.0
 * Host stand-in for ESP-IDF's esp_app_format.h (app image format docs). */
#pragma once
#include <stdint.h>

#define ESP_IMAGE_HEADER_MAGIC 0xE9

typedef enum {
    ESP_CHIP_ID_ESP32 = 0x0000,
    ESP_CHIP_ID_ESP32S3 = 0x0009,
    ESP_CHIP_ID_INVALID = 0xFFFF,
} __attribute__((packed)) esp_chip_id_t;

typedef struct {
    uint8_t magic;
    uint8_t segment_count;
    uint8_t spi_mode;
    uint8_t spi_speed: 4;
    uint8_t spi_size: 4;
    uint32_t entry_addr;
    uint8_t wp_pin;
    uint8_t spi_pin_drv[3];
    esp_chip_id_t chip_id;
    uint8_t min_chip_rev;
    uint16_t min_chip_rev_full;
    uint16_t max_chip_rev_full;
    uint8_t reserved[4];
    uint8_t hash_appended;
} __attribute__((packed)) esp_image_header_t;

typedef struct {
    uint32_t load_addr;
    uint32_t data_len;
} esp_image_segment_header_t;
