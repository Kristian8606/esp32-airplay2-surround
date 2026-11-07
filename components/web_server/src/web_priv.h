/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

/* Between the files of this component only. */

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "esp_http_server.h"

/* Reads len bytes of the request body: into buf (at least len bytes) when
 * keep, else through buf (buf_len bytes) and dropped. A receive timeout is
 * tried again, at most WEB_RECV_MAX_TIMEOUTS in all. Another error, the peer
 * closing early or too many timeouts: ESP_FAIL. Sends no response. */
#define WEB_RECV_MAX_TIMEOUTS 3
esp_err_t web_recv_body(httpd_req_t *req, char *buf, size_t buf_len,
                        size_t len, bool keep);
