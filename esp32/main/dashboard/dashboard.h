/*
 * Dashboard public API (CONFIG_HOMEHUB_DASHBOARD).
 *
 * Touch dashboard for the CYD: stocks / weather / calendar screens with
 * swipe and arrow navigation, a bridge-poll data provider, and a full-screen
 * takeover mode for images pushed by Muse (dismiss with the X button).
 *
 * All dashboard drawing happens on the dashboard task; the functions below
 * only change state and wake it, so they are safe from any task.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

// Call once at boot, after led_status_init().
void dashboard_init(void);

// Call when the Link session state changes. The dashboard activates on the
// first successful connection and stays up afterwards.
void dashboard_set_paired(bool paired);

// Link (control session) up or down, for the header's status dot.
void dashboard_set_link(bool up);

// True once the dashboard has drawn and kept running for a while (or never
// started). The OTA verifier waits for it before accepting a new image, so a
// firmware that crashes in the dashboard rolls back instead of looping.
bool dashboard_healthy(void);

// Start the on-screen touch calibration, or with `reset` forget the saved one.
void dashboard_calibrate(bool reset);

// Live quotes (see dash_quotes.h): set the watchlist, and a one-line status.
bool dashboard_quotes_configure(const char *symbols, const char **err);
void dashboard_quotes_status(char *buf, size_t n);

// Firmware update progress (0..100) for the on-screen progress bar; -1 when
// the update failed (back to the dashboard).
void dashboard_ota_progress(int pct);

// An image is about to be drawn full-screen (display.draw_url,
// dashboard.takeover, display.draw_sd). Stops dashboard rendering and waits
// for an in-progress frame to stop, so the image is not drawn over. Follow
// with dashboard_takeover_begin() on success or dashboard_takeover_end() on
// failure.
void dashboard_takeover_prepare(void);

// A pushed image finished drawing. Enters takeover mode with an X dismiss
// button.
void dashboard_takeover_begin(void);

// Leave takeover (or a pending takeover): X pressed, display.show_animation,
// or the image failed. The dashboard repaints.
void dashboard_takeover_end(void);

// Push data to a dashboard screen: "calendar" or "bridge" (stocks/weather).
// Returns false on bad input. Changed rows redraw if the screen is visible.
bool dashboard_data_set(const char *screen, const cJSON *data);
// Same, with the data as a JSON string.
bool dashboard_data_set_json(const char *screen, const char *json);

// Called by the bridge poller when fresh data is in the store.
void dashboard_data_updated(void);

// One-line diagnostics for remote debugging: touch, backlight, state, heap.
void dashboard_debug(char *buf, size_t n);

#ifdef __cplusplus
}
#endif
