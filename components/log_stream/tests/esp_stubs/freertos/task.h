#pragma once
#include "freertos/FreeRTOS.h"
typedef struct tskTaskControlBlock *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);
typedef enum { eNoAction = 0, eSetBits, eIncrement } eNotifyAction;
TaskHandle_t xTaskGetCurrentTaskHandle(void);
TickType_t xTaskGetTickCount(void);
void vTaskDelay(const TickType_t xTicksToDelay);
BaseType_t xTaskNotify(TaskHandle_t xTaskToNotify, uint32_t ulValue, eNotifyAction eAction);
BaseType_t xTaskNotifyWait(uint32_t ulBitsToClearOnEntry, uint32_t ulBitsToClearOnExit,
                           uint32_t *pulNotificationValue, TickType_t xTicksToWait);
