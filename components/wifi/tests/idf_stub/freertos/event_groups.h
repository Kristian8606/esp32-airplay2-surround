/*
 * SPDX-License-Identifier: Apache-2.0
 */
/* Stand-in for the ESP-IDF header of the same name: see README.md. */
#pragma once
#include "freertos/FreeRTOS.h"

typedef uint32_t EventBits_t;
typedef struct EventGroupDef_t *EventGroupHandle_t;
typedef struct {
  TickType_t x[2];
  void *p[5];
  uint8_t u[4];
} StaticEventGroup_t;

EventGroupHandle_t xEventGroupCreateStatic(StaticEventGroup_t *buf);
EventBits_t xEventGroupSetBits(EventGroupHandle_t g, EventBits_t bits);
EventBits_t xEventGroupClearBits(EventGroupHandle_t g, EventBits_t bits);
EventBits_t xEventGroupWaitBits(EventGroupHandle_t g, EventBits_t bits,
                                BaseType_t clear, BaseType_t all,
                                TickType_t ticks);
