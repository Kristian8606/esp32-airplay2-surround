#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

/* Firmware update from the body of an HTTP POST: writes it to the spare
 * application partition, checks it and makes it boot next. It sends no
 * response and does not restart; the caller does both. See README.md. */
esp_err_t ota_start_from_http(httpd_req_t *req);
