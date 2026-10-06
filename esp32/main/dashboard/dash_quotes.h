/*
 * Live stock quotes fetched by the board itself (CONFIG_HOMEHUB_DASHBOARD_
 * QUOTES): Finnhub's quote API over HTTPS, every ~15 s while the US market
 * is open, every minute pre-/after-market, every 30 min when closed. No
 * server of your own and no Muse VM in the loop.
 *
 * Each round runs on a short-lived task (its TLS stack and session are freed
 * afterwards) and is skipped while RAM is short or an image or firmware
 * update is downloading.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Load the API key and watchlist (NVS, else Kconfig). Call once.
void dash_quotes_init(void);

// Start a round if one is due. Call from the dashboard task on every wake-up;
// `allowed` is false while the network should stay quiet (image or firmware
// download in progress).
void dash_quotes_tick(bool allowed);

// Change the key and/or watchlist at runtime (saved to NVS); NULL leaves a
// value as is, "" for the key turns fetching off. Fetches at once. Returns
// false with *err on a bad watchlist.
bool dash_quotes_configure(const char *key, const char *symbols,
                           const char **err);

// One-line status for dashboard.debug / dashboard.stocks.
void dash_quotes_status(char *buf, size_t n);

#ifdef __cplusplus
}
#endif
