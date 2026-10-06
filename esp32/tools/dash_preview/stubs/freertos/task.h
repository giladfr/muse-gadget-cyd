#pragma once
#include "freertos/FreeRTOS.h"
typedef void *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);
BaseType_t xTaskCreate(TaskFunction_t f, const char *n, uint32_t st, void *a, UBaseType_t p, TaskHandle_t *h);
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t t);
BaseType_t xTaskNotifyGive(TaskHandle_t t);
void vTaskNotifyGiveFromISR(TaskHandle_t t, BaseType_t *w);
void vTaskDelay(TickType_t t);
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t t);
TickType_t xTaskGetTickCount(void);
void vTaskDelete(TaskHandle_t t);
