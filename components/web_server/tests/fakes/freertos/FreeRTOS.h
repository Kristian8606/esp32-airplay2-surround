/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdint.h>
typedef uint32_t TickType_t;
#define configTICK_RATE_HZ 100
#define pdMS_TO_TICKS(ms) ((TickType_t)(((uint64_t)(ms) * configTICK_RATE_HZ) / 1000))
