/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdint.h>

#include "esp_err.h"

/* The web server of both firmwares: the shared pages and APIs, and each
 * firmware's own through web_server_internal.h. See spec/spec.md. */

/* Starts the server on port (80) once, after Wi-Fi is up. */
esp_err_t web_server_start(uint16_t port);
