/*
 * Wall clock for the dashboard: SNTP plus a POSIX time zone, with the
 * bridge's "now" field as a fallback until SNTP answers.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

// Set the time zone and start SNTP. Call once, after Wi-Fi is up.
void dash_clock_start(void);

// The clock has been set (by SNTP or the bridge).
bool dash_clock_valid(void);

// Local time; false if the clock is not set.
bool dash_clock_local(struct tm *out);

// Israel time (Asia/Jerusalem); false if the clock is not set.
bool dash_clock_israel(struct tm *out);

// Seed the clock from a trusted unix time (the bridge's "now") if nothing has
// set it yet. SNTP corrects it later.
void dash_clock_seed(int64_t unix_s);

#ifdef __cplusplus
}
#endif
