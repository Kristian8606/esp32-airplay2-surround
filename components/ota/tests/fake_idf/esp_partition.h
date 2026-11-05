/* SPDX-License-Identifier: Apache-2.0
 * Host stand-in for ESP-IDF's esp_partition.h. */
#pragma once
#include <stdint.h>

typedef struct {
    uint32_t address;
    uint32_t size;
    char label[17];
} esp_partition_t;
