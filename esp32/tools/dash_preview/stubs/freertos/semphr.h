#pragma once
#include "freertos/FreeRTOS.h"
typedef void *SemaphoreHandle_t;
static inline SemaphoreHandle_t xSemaphoreCreateMutex(void){static int m; return &m;}
static inline SemaphoreHandle_t xSemaphoreCreateBinary(void){static int m; return &m;}
static inline BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t t){(void)s;(void)t;return 1;}
static inline BaseType_t xSemaphoreGive(SemaphoreHandle_t s){(void)s;return 1;}
