/* SPDX-License-Identifier: Apache-2.0 */
/* Host stand-in for FreeRTOS mutexes, on pthreads. */
#pragma once

#include "freertos/FreeRTOS.h"

typedef struct {
  pthread_mutex_t mutex;
} StaticSemaphore_t;

typedef StaticSemaphore_t *SemaphoreHandle_t;

static inline SemaphoreHandle_t xSemaphoreCreateMutexStatic(
    StaticSemaphore_t *buffer) {
  pthread_mutex_init(&buffer->mutex, NULL);
  return buffer;
}

static inline BaseType_t xSemaphoreTake(SemaphoreHandle_t sem,
                                        TickType_t ticks) {
  (void)ticks;
  return pthread_mutex_lock(&sem->mutex) == 0 ? pdTRUE : pdFALSE;
}

static inline BaseType_t xSemaphoreGive(SemaphoreHandle_t sem) {
  return pthread_mutex_unlock(&sem->mutex) == 0 ? pdTRUE : pdFALSE;
}
