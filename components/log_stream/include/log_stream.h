#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

/* Live log for the browser page /logs: everything logged through ESP-IDF
 * logging goes to the browsers connected to the WebSocket /ws/logs, the
 * recent history first. The UART log is unchanged. See spec/spec.md. */

/* Starts capturing. Call once, early, before the web server starts. */
esp_err_t log_stream_init(void);

/* Adds /ws/logs to the running web server and starts sending. */
esp_err_t log_stream_register(httpd_handle_t server);
