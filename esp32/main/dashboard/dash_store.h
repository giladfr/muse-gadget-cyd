/*
 * Dashboard data store: stocks, weather, calendar events, and a wall-clock
 * estimate derived from the bridge's `updated` timestamp.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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
    int64_t updated_ts;     // bridge `updated`, unix seconds
    int64_t updated_ticks;  // xTaskGetTickCount() when updated arrived
} dash_store_t;

// Singleton, mutex-guarded. Copy out under lock, then render.
dash_store_t *dash_store_lock(void);
void dash_store_unlock(void);

// Replace stocks/weather from bridge JSON. Returns false on parse failure.
bool dash_store_set_bridge(const char *json, size_t len);
// Replace calendar events from dashboard.data JSON.
bool dash_store_set_calendar(const char *json, size_t len);

// Seconds since the bridge data arrived (INT64_MAX if never).
int64_t dash_store_age_s(void);
// Weekday name ("Tue") for the bridge's `updated` date, in America/Chicago.
void dash_store_day_label(char *out, size_t out_len);

#ifdef __cplusplus
}
#endif
