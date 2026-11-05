/* SPDX-License-Identifier: Apache-2.0
 * Host stand-in for ESP-IDF's esp_timer.h. */
#pragma once
#include <stdint.h>

int64_t esp_timer_get_time(void);
