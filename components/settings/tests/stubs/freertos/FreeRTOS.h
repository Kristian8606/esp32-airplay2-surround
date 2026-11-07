/* SPDX-License-Identifier: Apache-2.0 */
/* Host stand-in for the FreeRTOS calls the settings use, on pthreads. */
#pragma once

#include <pthread.h>
#include <stdint.h>

typedef uint32_t TickType_t;
typedef int BaseType_t;

#define portMAX_DELAY ((TickType_t)0xffffffffu)
#define pdTRUE 1
#define pdFALSE 0
