/*
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

/* The few esp_err.h names the host-built code uses, with ESP-IDF's values. */

#include <stdint.h>

typedef int esp_err_t;

#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_INVALID_SIZE 0x104
#define ESP_ERR_NOT_FOUND 0x105
#define ESP_ERR_TIMEOUT 0x107

const char *esp_err_to_name(esp_err_t code);
void abort(void);
#define ESP_ERROR_CHECK(x)      \
  do {                          \
    esp_err_t rc_ = (x);        \
    if (rc_ != ESP_OK) {        \
      abort();                  \
    }                           \
  } while (0)
