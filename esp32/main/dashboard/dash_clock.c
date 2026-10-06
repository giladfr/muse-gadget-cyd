/*
 * Dashboard clock implementation.
 */
#include "dash_clock.h"

#include <stdlib.h>
#include <sys/time.h>

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

void dash_clock_seed(int64_t unix_s) {
    if (unix_s <= VALID_AFTER || dash_clock_valid()) return;
    struct timeval tv = {.tv_sec = (time_t)unix_s, .tv_usec = 0};
    settimeofday(&tv, NULL);
    ESP_LOGI(TAG, "clock seeded from bridge: %lld", (long long)unix_s);
}
