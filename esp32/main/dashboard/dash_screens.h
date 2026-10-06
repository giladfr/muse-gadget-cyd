/*
 * Dashboard screens: stocks, weather, calendar + bottom nav bar.
 * Each screen renders in horizontal strips via draw_strip().
 */
#pragma once

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

// Render one strip (screen rows [sy0, sy0+sh)) of screen s into buf.
void dash_screen_draw_strip(dash_screen_t s, uint16_t *buf, int sy0, int sh);

#ifdef __cplusplus
}
#endif
