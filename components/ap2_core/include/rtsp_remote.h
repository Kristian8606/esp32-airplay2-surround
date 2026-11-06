/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* Remote commands to the sender over the event channel. See docs/rtsp-spec.md,
 * "Event channel". Numbers 0 to 5 are media remote commands (play, pause,
 * toggle, stop, next, previous); 6 and 7 change the volume. */
#define RTSP_REMOTE_VOLUME_DOWN 6U
#define RTSP_REMOTE_VOLUME_UP 7U

typedef enum {
  RTSP_REMOTE_IDLE, RTSP_REMOTE_QUEUED, RTSP_REMOTE_SENT,
  RTSP_REMOTE_ACKNOWLEDGED, RTSP_REMOTE_REJECTED,
  RTSP_REMOTE_TIMEOUT, RTSP_REMOTE_FAILED, RTSP_REMOTE_DISCONNECTED
} rtsp_remote_result_t;

typedef struct {
  bool connected;     /* an encrypted event connection exists */
  uint32_t id;        /* the last command's id, 0 before the first */
  uint8_t command;    /* its number */
  rtsp_remote_result_t result;
  int response_code;  /* the sender's RTSP status, 0 without a reply */
} rtsp_remote_status_t;

/* Queues one command. ESP_ERR_NOT_FOUND: no encrypted event connection;
 * ESP_ERR_INVALID_STATE: the previous command has no result yet;
 * ESP_ERR_INVALID_ARG: number over 7. *id gets the command's id. */
esp_err_t rtsp_remote_enqueue(uint8_t command, uint32_t *id);
void rtsp_remote_get_status(rtsp_remote_status_t *status);
/* "idle", "queued", "sent", "acknowledged", "rejected", "timeout",
 * "failed", "disconnected". */
const char *rtsp_remote_result_name(rtsp_remote_result_t result);
