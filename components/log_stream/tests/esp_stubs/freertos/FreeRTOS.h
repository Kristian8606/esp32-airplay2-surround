#pragma once
#include <stdint.h>
typedef int BaseType_t;
typedef unsigned int UBaseType_t;
typedef uint32_t TickType_t;
#define configSTACK_DEPTH_TYPE uint32_t
#define portMAX_DELAY ((TickType_t)0xffffffffUL)
#define pdPASS 1
#define pdFAIL 0
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
typedef struct { uint32_t owner; uint32_t count; } portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED { 0, 0 }
void vPortEnterCritical(portMUX_TYPE *mux);
void vPortExitCritical(portMUX_TYPE *mux);
#define portENTER_CRITICAL(mux) vPortEnterCritical(mux)
#define portEXIT_CRITICAL(mux) vPortExitCritical(mux)
#define portENTER_CRITICAL_SAFE(mux) vPortEnterCritical(mux)
#define portEXIT_CRITICAL_SAFE(mux) vPortExitCritical(mux)
