/*
 * XPT2046 resistive touch driver for the CYD (shared SPI bus with the
 * display). Polling interface with tap and swipe detection for the dashboard.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

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

// Init the touch controller on the shared SPI bus. Returns false if the
// controller does not answer (dashboard still works; touch is disabled).
bool dash_touch_init(void);

// Poll for a touch event. Call every ~50ms from the dashboard task.
dash_touch_t dash_touch_poll(void);

// One-line diagnostics for remote debugging: init state, IRQ level, raw ADC.
void dash_touch_debug(char *buf, size_t n);

#ifdef __cplusplus
}
#endif
