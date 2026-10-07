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

#include "dash_cards.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DASH_SCREEN_STOCKS = 0,
    DASH_SCREEN_WEATHER = 1,
    DASH_SCREEN_CALENDAR = 2,      // today
    DASH_SCREEN_CALENDAR_TOM = 3,  // tomorrow
    DASH_SCREEN_CLOCK = 4,
    DASH_SCREEN_COUNT = 5,
} dash_screen_t;

const char *dash_screen_name(dash_screen_t s);

// Screens in the rotation: the built-in ones, then Muse's cards
// (DASH_SCREEN_COUNT + card index).
int dash_screen_total(void);

// Connection state shown in the header.
typedef struct {
    bool link;      // Link session up
    int wifi_bars;  // 0-3 by signal strength, -1 = not associated
} dash_status_t;

// The content area between the header and the footer (screen rows).
#define DASH_CONTENT_Y0 32
#define DASH_CONTENT_Y1 218

// Build the frame for screen s from the store. *y0/*y1 receive the screen
// rows [y0, y1) that differ from the previous frame (y0 == y1 when nothing
// changed); `full` marks the whole screen dirty.
void dash_screen_prepare(int s, const dash_status_t *st, bool full, int *y0,
                         int *y1);

// What a tap at (x, y) on the prepared frame hits: a card button (its index),
// the banner, or nothing.
#define DASH_HIT_NONE (-1)
#define DASH_HIT_BANNER (-2)
int dash_screen_hit(int x, int y);

// The card on screen, if one is: its id, title and buttons (valid until the
// next prepare).
bool dash_screen_card(const char **id, const char **title,
                      const dash_card_button_t **buttons, int *n);
// The banner on screen, if one is.
bool dash_screen_banner(dash_banner_t *out);

// True while the last prepared frame has something animating (a price-change
// flash fading out): prepare again soon.
bool dash_screen_animating(void);

// A full-screen message instead of a dashboard screen (touch calibration,
// firmware update): a title, a line of text, a hint, a crosshair at (cx, cy)
// unless cx < 0, and a progress bar unless progress < 0. The next
// dash_screen_prepare() repaints everything.
void dash_screen_prepare_message(const char *title, const char *msg,
                                 const char *hint, int cx, int cy,
                                 int progress);

// The boot splash, `ms` into its animation (dash_splash_begin() first), with
// the firmware version under the title.
void dash_screen_prepare_splash(int ms, const char *version);

// Render one strip (screen rows [sy0, sy0+sh)) of the prepared frame, or of
// the one before it (the outgoing screen of a slide transition).
void dash_screen_draw_strip(uint16_t *buf, int sy0, int sh);
void dash_screen_draw_strip_prev(uint16_t *buf, int sy0, int sh);

#ifdef __cplusplus
}
#endif
