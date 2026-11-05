/*
 * SPDX-License-Identifier: Apache-2.0
 */
/* Stand-in for the ESP-IDF header of the same name: see README.md. */
#pragma once
#include "freertos/FreeRTOS.h"

typedef struct QueueDefinition *SemaphoreHandle_t;
typedef struct {
  void *p[10];
  uint32_t u[8];
} StaticSemaphore_t;

SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t *buf);
BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, TickType_t ticks);
BaseType_t xSemaphoreGive(SemaphoreHandle_t sem);
