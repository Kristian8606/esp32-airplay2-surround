#pragma once
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
BaseType_t xTaskCreateWithCaps(TaskFunction_t pvTaskCode, const char *const pcName,
                               configSTACK_DEPTH_TYPE usStackDepth, void *const pvParameters,
                               UBaseType_t uxPriority, TaskHandle_t *pvCreatedTask,
                               UBaseType_t uxMemoryCaps);
void vTaskDeleteWithCaps(TaskHandle_t xTaskToDelete);
