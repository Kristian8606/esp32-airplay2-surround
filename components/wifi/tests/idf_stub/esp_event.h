/*
 * SPDX-License-Identifier: Apache-2.0
 */
/* Stand-in for the ESP-IDF header of the same name: see README.md. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

typedef const char *esp_event_base_t;
typedef void *esp_event_handler_instance_t;
typedef void (*esp_event_handler_t)(void *arg, esp_event_base_t base,
                                    int32_t id, void *data);
#define ESP_EVENT_ANY_ID (-1)
#define ESP_EVENT_DEFINE_BASE(id) esp_event_base_t const id = #id

extern esp_event_base_t const WIFI_EVENT;
extern esp_event_base_t const IP_EVENT;

esp_err_t esp_event_loop_create_default(void);
esp_err_t esp_event_post(esp_event_base_t base, int32_t id, const void *data,
                         size_t size, TickType_t ticks);
esp_err_t esp_event_handler_instance_register(
    esp_event_base_t base, int32_t id, esp_event_handler_t handler, void *arg,
    esp_event_handler_instance_t *instance);
