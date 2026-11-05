/*
 * SPDX-License-Identifier: Apache-2.0
 */
/* Stand-in for the ESP-IDF header of the same name: see README.md. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef uint32_t TickType_t;
typedef int BaseType_t;
#define pdTRUE 1
#define pdFALSE 0
#define portMAX_DELAY ((TickType_t)0xffffffffUL)
#define configTICK_RATE_HZ 1000
#define pdMS_TO_TICKS(ms) \
  ((TickType_t)(((uint64_t)(ms) * configTICK_RATE_HZ) / 1000U))
#define BIT0 0x01u
#define BIT1 0x02u

typedef struct {
  uint32_t owner;
  uint32_t count;
} portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED {.owner = 0xB33FFFFFu, .count = 0}
void vPortEnterCritical(portMUX_TYPE *mux);
void vPortExitCritical(portMUX_TYPE *mux);
#define taskENTER_CRITICAL(mux) vPortEnterCritical(mux)
#define taskEXIT_CRITICAL(mux) vPortExitCritical(mux)
