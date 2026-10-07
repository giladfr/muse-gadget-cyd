/*
 * Dashboard clock implementation.
 */
#include "dash_clock.h"

#include <stdint.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>

#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "sdkconfig.h"

static const char *TAG = "dash.clock";

// Anything before this is the boot-time epoch, not a real clock.
#define VALID_AFTER 1704067200  // 2024-01-01

static bool s_started;
static bool s_tz_set;

static void tz_once(void) {
    if (s_tz_set) return;
    s_tz_set = true;
    setenv("TZ", CONFIG_HOMEHUB_DASHBOARD_TZ, 1);
    tzset();
}

static void on_sync(struct timeval *tv) {
    ESP_LOGI(TAG, "SNTP time %lld", (long long)tv->tv_sec);
}

void dash_clock_start(void) {
    if (s_started) return;
    s_started = true;
    tz_once();
    esp_sntp_config_t cfg =
        ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_HOMEHUB_DASHBOARD_NTP_SERVER);
    cfg.sync_cb = on_sync;
    esp_err_t err = esp_netif_sntp_init(&cfg);
    ESP_LOGI(TAG, "TZ %s, SNTP %s: %s", CONFIG_HOMEHUB_DASHBOARD_TZ,
             CONFIG_HOMEHUB_DASHBOARD_NTP_SERVER, esp_err_to_name(err));
}

bool dash_clock_valid(void) {
    return time(NULL) > VALID_AFTER;
}

bool dash_clock_local(struct tm *out) {
    time_t now = time(NULL);
    if (now <= VALID_AFTER) return false;
    tz_once();
    localtime_r(&now, out);
    return true;
}

// Israel time, computed rather than by switching TZ: setenv() on newlib
// never frees the old value when the new one is longer, so swapping TZ back
// and forth leaked ~30 bytes of internal RAM per call (every redraw), and
// other tasks could read local time while TZ pointed at Israel.
//
// Israel Standard Time is UTC+2; daylight time (UTC+3) runs from the Friday
// before the last Sunday of March, 02:00 local (00:00 UTC), to the last
// Sunday of October, 02:00 local daylight time (23:00 UTC the day before).

// Days since 1970-01-01 of a proleptic Gregorian date (Howard Hinnant).
static int64_t days_from_civil(int y, int m, int d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int yoe = (int)(y - era * 400);
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

// Day number of the last Sunday of a 31-day month (March, October).
static int64_t last_sunday(int year, int month) {
    int64_t d = days_from_civil(year, month, 31);
    int wday = (int)((d + 4) % 7);  // 1970-01-01 was a Thursday (4)
    return d - wday;
}

void dash_clock_israel_at(time_t utc, struct tm *out) {
    struct tm u;
    gmtime_r(&utc, &u);
    int year = u.tm_year + 1900;
    int64_t dst_start = (last_sunday(year, 3) - 2) * 86400;    // Fri 00:00 UTC
    int64_t dst_end = last_sunday(year, 10) * 86400 - 3600;    // Sat 23:00 UTC
    bool dst = (int64_t)utc >= dst_start && (int64_t)utc < dst_end;
    time_t local = utc + (dst ? 3 : 2) * 3600;
    gmtime_r(&local, out);
    out->tm_isdst = dst;
}

bool dash_clock_israel(struct tm *out) {
    time_t now = time(NULL);
    if (now <= VALID_AFTER) return false;
    dash_clock_israel_at(now, out);
    return true;
}

void dash_clock_seed(int64_t unix_s) {
    if (unix_s <= VALID_AFTER || dash_clock_valid()) return;
    struct timeval tv = {.tv_sec = (time_t)unix_s, .tv_usec = 0};
    settimeofday(&tv, NULL);
    ESP_LOGI(TAG, "clock seeded from bridge: %lld", (long long)unix_s);
}
