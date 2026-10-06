/*
 * Dashboard data store: stocks, weather and calendar events, plus how fresh
 * the stock quotes are.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DASH_MAX_STOCKS  8
#define DASH_MAX_EVENTS  8
#define DASH_MAX_FORECAST 4

typedef struct {
    char symbol[16];
    double price;
    double change;
    double change_pct;
    char market[16];   // "Open" / "Closed" / ...
} dash_stock_t;

typedef struct {
    char day[8];       // "Tue"
    int high;
    int low;
    int code;          // Open-Meteo weather code
} dash_forecast_t;

typedef struct {
    int temp;
    int feels;
    char desc[24];
    int humidity;
    int wind;          // mph
    dash_forecast_t forecast[DASH_MAX_FORECAST];
    int forecast_n;
} dash_weather_t;

typedef struct {
    char time[16];     // "5:45 PM"
    char title[48];
} dash_event_t;

typedef struct {
    dash_stock_t stocks[DASH_MAX_STOCKS];
    int n_stocks;
    dash_weather_t weather;
    bool weather_valid;
    dash_event_t events[DASH_MAX_EVENTS];
    int n_events;
    char events_label[32];  // e.g. "Tuesday, Oct 6"
    // Quote freshness: how old the quotes already were when they arrived
    // (bridge `now` - `stocks_updated`), and when they arrived
    // (esp_timer_get_time()). stocks_rx_us == 0 means no quotes yet.
    int64_t stocks_age_at_rx_s;
    int64_t stocks_rx_us;
} dash_store_t;

// Copy the whole store out under its lock.
void dash_store_snapshot(dash_store_t *out);

// Merge bridge JSON ({"stocks": [...], "weather": {...}, ...}). A missing or
// empty "stocks" array and a missing "weather" object keep the previous data,
// so a failed upstream fetch never blanks the screen. Returns false if the
// document is not an object.
bool dash_store_set_bridge(const cJSON *root);
// Replace calendar events ({"label": "...", "events": [...]}).
bool dash_store_set_calendar(const cJSON *root);

// Age of the quotes in a snapshot, in seconds; INT64_MAX if there are none.
int64_t dash_store_stocks_age_s(const dash_store_t *s);

#ifdef __cplusplus
}
#endif
