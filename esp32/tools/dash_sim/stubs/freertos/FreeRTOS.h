// Simulator FreeRTOS: tasks are threads, ticks are milliseconds.
#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef int BaseType_t;
typedef uint32_t TickType_t;
typedef unsigned UBaseType_t;
#define pdFALSE 0
#define pdTRUE 1
#define pdPASS 1
#define portMAX_DELAY 0xffffffffu
#define configTICK_RATE_HZ 1000
#define pdMS_TO_TICKS(x) ((TickType_t)(x))
#define portYIELD_FROM_ISR(x) (void)(x)
