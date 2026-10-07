#pragma once
#include "freertos/FreeRTOS.h"
typedef struct sim_task *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);
BaseType_t xTaskCreate(TaskFunction_t f, const char *name, uint32_t stack, void *arg,
                       UBaseType_t prio, TaskHandle_t *out);
void vTaskDelete(TaskHandle_t t);
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t wait);
BaseType_t xTaskNotifyGive(TaskHandle_t t);
void vTaskNotifyGiveFromISR(TaskHandle_t t, BaseType_t *woken);
void vTaskDelay(TickType_t t);
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t t);
TickType_t xTaskGetTickCount(void);
