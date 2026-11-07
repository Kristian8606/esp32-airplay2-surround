/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "freertos/FreeRTOS.h"
/* The fake moves the fake clock on instead of sleeping. */
void vTaskDelay(TickType_t ticks);
