/*
 * SPDX-FileCopyrightText: 2026 Kristian8606
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>

#include "esp_err.h"

/* Captive DNS for the setup access point: every name resolves to one IPv4
 * address. See spec/spec.md, "dns_server". */

/* redirect_ip: IPv4 address in network byte order. ESP_ERR_INVALID_STATE
 * while it already runs. */
esp_err_t dns_server_start(uint32_t redirect_ip);

/* Returns when the server has ended and UDP port 53 is free. */
void dns_server_stop(void);
