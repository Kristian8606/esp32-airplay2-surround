/*
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
/* The time pulse (1.3): a square wave on the WAKEUP line with a rising edge
 * every SURROUND_TIME_INTERVAL_MS, made in hardware only:
 *
 *   pulse GPTimer, alarm every half period, auto reload
 *     --ETM--> GPIO TOGGLE (CONFIG_GW_C6_WAKEUP_GPIO)
 *     --ETM--> EN_ALARM of the same timer (the hardware disables the alarm
 *              when it fires; the driver would re-enable it in an interrupt)
 *
 * The C6 has two GPTimers: one is the clock (c6_clock.h), this is the other.
 * Both run from PLL_F80M at 40 MHz, so the C6 clock at every edge is known
 * exactly: edge n = edge 0 + n * period. The period is a whole number of
 * ticks, fixed in hardware for one boot: C6_PULSE_PERIOD_TICKS plus what the
 * auto reload adds per alarm. Measured 5 000 006, on some boots 5 000 004.
 *
 * Edge 0 and the period are measured at the start: the edge of the
 * pin --ETM--> CAPTURE of the C6 clock, over 9 edges (c6_pulse.c, calibrate:
 * a capture is now and then one tick late), then that channel is removed.
 * It must be removed: a software read of the clock
 * (gptimer_get_raw_count) latches the counter into the same register as the
 * ETM capture, so later captures could not be told apart from reads. */
#include <stdbool.h>
#include <stdint.h>

#include "c6_clock.h"
#include "esp_err.h"
#include "surround_proto.h"

#define C6_PULSE_PERIOD_TICKS \
  ((uint64_t)C6_CLOCK_HZ / 1000U * SURROUND_TIME_INTERVAL_MS)

/* Needs c6_clock_start(); call it before anything else reads the C6 clock
 * (it takes about 1.2 s: nine edges are measured; up to three tries). Safe to call again.
 * On an error nothing stays half set up and the line stays low. */
esp_err_t c6_pulse_start(void);

/* The C6 clock at the last rising edge and its number (0 = first edge).
 * false when the pulse is off. Any task. */
bool c6_pulse_last_edge(uint64_t *edge_c6_ticks, uint32_t *seq);

/* The measured period in C6 ticks (0 when off). */
uint64_t c6_pulse_period_ticks(void);
