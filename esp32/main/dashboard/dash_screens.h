/*
 * Dashboard screens: stocks, weather, calendar + bottom nav bar.
 *
 * Rendering is two-phase: dash_screen_prepare() snapshots the store once and
 * formats every string into a frame, then dash_screen_draw_strip() paints
 * horizontal strips of that frame. Comparing a frame with the previous one
 * gives the rows that actually changed, so a quote update or the header's
 * age label redraws only those bands.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DASH_SCREEN_STOCKS = 0,
    DASH_SCREEN_WEATHER = 1,
    DASH_SCREEN_CALENDAR = 2,
    DASH_SCREEN_COUNT = 3,
} dash_screen_t;

const char *dash_screen_name(dash_screen_t s);

// Build the frame for screen s from the store. *y0/*y1 receive the screen
// rows [y0, y1) that differ from the previous frame (y0 == y1 when nothing
// changed); `full` marks the whole screen dirty.
void dash_screen_prepare(dash_screen_t s, bool full, int *y0, int *y1);

// Render one strip (screen rows [sy0, sy0+sh)) of the prepared frame.
void dash_screen_draw_strip(uint16_t *buf, int sy0, int sh);

#ifdef __cplusplus
}
#endif
