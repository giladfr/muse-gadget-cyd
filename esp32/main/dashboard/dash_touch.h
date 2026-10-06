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
    DASH_TOUCH_LONG,       // held still for DASH_TOUCH_LONG_MS (fires once)
} dash_touch_event_t;

#define DASH_TOUCH_LONG_MS 5000

typedef struct {
    dash_touch_event_t type;
    int x, y;          // screen coords for TAP
    int raw_x, raw_y;  // filtered raw readings at the touch-down point
} dash_touch_t;

// Raw-to-screen mapping: screen X comes from raw axis src_x (0 = X
// channel, 1 = Y channel), scaled so raw x_lo is screen 0 and raw x_hi is
// screen DASH_W (x_hi < x_lo inverts). Same for Y.
typedef struct {
    uint8_t src_x, src_y;
    int16_t x_lo, x_hi, y_lo, y_hi;
} dash_touch_cal_t;

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

// Current mapping (NVS calibration, else the Kconfig defaults).
void dash_touch_get_cal(dash_touch_cal_t *out);

// Build a mapping from three taps on screen targets t[0..2] (t[1] to the
// right of t[0], t[2] below it) with raw readings r[0..2]. Returns false if
// the points don't make sense (same axis, too close).
bool dash_touch_cal_compute(const int t[3][2], const int r[3][2],
                            dash_touch_cal_t *out);

// Use and save a mapping; NULL forgets the saved one (Kconfig defaults).
void dash_touch_set_cal(const dash_touch_cal_t *cal);

// One-line diagnostics for remote debugging: init state, IRQ level, last raw
// readings. Safe to call from any task (it does not touch the controller).
void dash_touch_debug(char *buf, size_t n);

#ifdef __cplusplus
}
#endif
