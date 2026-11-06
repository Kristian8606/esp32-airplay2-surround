/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

/* The AirPlay 2 control server of the Gateway (TCP 7000). See docs/rtsp-spec.md. */

/* Starts serving senders; ESP_OK also when it already runs. */
esp_err_t rtsp_server_start(void);

/* Ends the session, closes every connection and the listener, and waits
 * until rtsp_server_is_idle() is true, at most 2 s. Never from the server's
 * own tasks. */
void rtsp_server_stop(void);

/* True when nothing of the server runs any more. */
bool rtsp_server_is_idle(void);
