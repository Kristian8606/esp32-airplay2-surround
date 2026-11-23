/*
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
/* PTP timestamps at receive (1.5).
 *
 * A callback in front of ESP-Hosted's frames from the air to the host
 * (eh_cp_set_traffic_tx_cb(ESP_STA_IF), the public co-processor API):
 * it reads the C6 clock first, then, for an IPv4 UDP datagram to port 319 or
 * 320 with a PTPv2 header, keeps messageType, sequenceId, sourcePortIdentity
 * and that clock value. The frame always goes on to the host unchanged.
 * apple_clock speaks IPv4 only, so IPv6 is not looked at.
 *
 * Sends are not timestamped here (1.6, decided 2026-11-12): the TX done
 * callback gives the encrypted 802.11 frame, so it cannot be matched to a
 * PTP message (0.9). Delay_Req keeps its send time from the P4 (s.t3 in
 * apple_clock), as on the S3. */
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

typedef struct {
  uint8_t type;       /* PTP messageType (low 4 bits of the first byte) */
  uint16_t seq;       /* PTP sequenceId */
  uint8_t src_id[10]; /* PTP sourcePortIdentity (clockIdentity + port) */
  uint64_t c6_ticks;  /* C6 clock when the frame reached the callback */
} c6_ptp_rec_t;

#define C6_PTP_RING 32

/* Installs the callback (and keeps it installed: a watcher puts it back if
 * ESP-Hosted sets its default again, e.g. after a new init). */
esp_err_t c6_ptp_start(void);

/* After a record is kept, xTaskNotify(task, bits, eSetBits) (from the frame
 * path, so the task should only wake up and take). NULL: no notification. */
void c6_ptp_set_notify(TaskHandle_t task, uint32_t bits);

/* Takes up to max records, oldest first. Any task. */
size_t c6_ptp_take(c6_ptp_rec_t *out, size_t max);

typedef struct {
  uint32_t frames;  /* frames seen */
  uint32_t records; /* PTP records kept */
  uint32_t dropped; /* records lost: the ring was full */
} c6_ptp_stats_t;
void c6_ptp_get_stats(c6_ptp_stats_t *out);
