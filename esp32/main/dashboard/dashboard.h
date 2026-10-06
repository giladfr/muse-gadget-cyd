/*
 * Dashboard public API (CONFIG_HOMEHUB_DASHBOARD).
 *
 * Touch dashboard for the CYD: stocks / weather / calendar screens with
 * swipe and arrow navigation, a bridge-poll data provider, and a full-screen
 * takeover mode for images pushed by Muse (dismiss with the X button).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Call once at boot, after led_status_init().
void dashboard_init(void);

// Call when the Link session state changes. The dashboard activates on the
// first successful connection and stays up afterwards.
void dashboard_set_paired(bool paired);

// A pushed image finished drawing (display.draw_url / dashboard.takeover).
// Enters takeover mode with an X dismiss button.
void dashboard_takeover_begin(void);

// Leave takeover mode (X pressed, timeout, or display.show_animation).
void dashboard_takeover_end(void);

// Push JSON data to a dashboard screen ("calendar"). Returns false on bad
// input. Redraws the screen if it is currently visible.
bool dashboard_data_set(const char *screen, const char *json);

// One-line touch diagnostics for remote debugging.
void dashboard_debug_touch(char *buf, size_t n);

#ifdef __cplusplus
}
#endif
