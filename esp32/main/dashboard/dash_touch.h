/*
 * XPT2046 resistive touch driver for the CYD. On the ESP32-2432S028R the
 * touch controller has its own pins (CLK 25, MOSI 32, MISO 39, CS 33,
 * IRQ 36), not the display's SPI bus, so it is bit-banged: the chip tops out
 * at ~2 MHz and a few dozen bits per sample cost nothing.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DASH_TOUCH_NONE,
    DASH_TOUCH_TAP,        // quick touch; x/y valid
    DASH_TOUCH_SWIPE_LEFT,
    DASH_TOUCH_SWIPE_RIGHT,
} dash_touch_event_t;

typedef struct {
    dash_touch_event_t type;
    int x, y;  // screen coords for TAP
} dash_touch_t;

// Init the touch controller. A pen-down interrupt sends `notify` a task
// notification (xTaskNotifyGive) so the dashboard task can sleep between
// touches. Returns false if the pins cannot be configured.
bool dash_touch_init(TaskHandle_t notify);

// Sample the panel and run gesture detection. Call from the dashboard task
// after a notification, and every DASH_TOUCH_FAST_MS while
// dash_touch_tracking() is true.
dash_touch_t dash_touch_poll(void);

// True when pen-down interrupts are wired up; otherwise the caller must poll
// every ~50 ms to see touches at all.
bool dash_touch_irq_driven(void);

// A touch is in progress (or just ended): poll quickly.
bool dash_touch_tracking(void);
#define DASH_TOUCH_FAST_MS 10

// One-line diagnostics for remote debugging: init state, IRQ level, last raw
// readings. Safe to call from any task (it does not touch the controller).
void dash_touch_debug(char *buf, size_t n);

#ifdef __cplusplus
}
#endif
