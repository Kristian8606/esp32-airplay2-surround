/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "esp_err.h"
#include "esp_http_server.h"

/* Reset cannot race the startup tasks that create identity and services. */
void homekit_reset_mark_ready(void);
esp_err_t homekit_reset_register(httpd_handle_t server);
/* HomeKit removal from Apple Home: ap2_port_homekit_reset_schedule() in
 * ap2_port.h. */
