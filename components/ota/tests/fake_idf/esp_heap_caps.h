/* SPDX-License-Identifier: Apache-2.0
 * Host stand-in for ESP-IDF's esp_heap_caps.h. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#define MALLOC_CAP_8BIT      (1 << 2)
#define MALLOC_CAP_SPIRAM    (1 << 10)
#define MALLOC_CAP_INTERNAL  (1 << 11)

void *heap_caps_malloc(size_t size, uint32_t caps);
void heap_caps_free(void *ptr);
