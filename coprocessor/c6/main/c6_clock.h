/*
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
/* The C6 clock (1.2): one GPTimer, started once at boot, counting up and never
 * reloaded. Every time in this firmware is a count of it ("c6_ticks").
 *
 * Source PLL_F80M (locked to the C6 crystal: the crystal's accuracy) divided
 * by 2: 40 MHz, 25 ns per tick. The counter has 54 bits (about 14 years). */
#include <stdint.h>

#include "driver/gptimer.h"
#include "esp_err.h"

#define C6_CLOCK_HZ 40000000U

/* Creates and starts the timer. Safe to call again (ESP_OK). On an error the
 * clock stays off and c6_clock_now() returns 0. */
esp_err_t c6_clock_start(void);

/* The current count, or 0 when the clock is off. From a task or an ISR, but
 * not from an IRAM ISR while the cache is off (the call is in flash). */
uint64_t c6_clock_now(void);

/* The timer itself, for ETM (1.3) and capture; NULL when the clock is off. */
gptimer_handle_t c6_clock_timer(void);
