/*
 * Live stock quotes implementation.
 */
#include "dash_quotes.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "dash_clock.h"
#include "dash_store.h"
#include "dashboard.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "sdkconfig.h"

static const char *TAG = "dash.quotes";

#if CONFIG_HOMEHUB_DASHBOARD_QUOTES

#define URL_FMT "https://finnhub.io/api/v1/quote?symbol=%s&token=%s"
#define MAX_SYMBOLS DASH_MAX_STOCKS
#define KEY_MAX 64
// Fits the URL with the longest symbol and key (and keeps -Wformat-truncation
// quiet, which cannot see that symbols are at most 11 characters).
#define URL_MAX 256
#define SYMS_MAX 96
// Finnhub's free tier allows 60 calls a minute; stay under 50.
#define CALLS_PER_MIN 50
#define EXTENDED_S 60
#define CLOSED_S (30 * 60)
#define RETRY_S 30
#define RATE_LIMIT_S 60
// While quotes keep arriving, the board owns them and pushed stocks are
// ignored; after this long without one, pushes are accepted again.
#define OWN_WINDOW_S (45 * 60)
// A TLS session needs a 16 KB receive buffer plus handshake state, and the
// round's task an 8 KB stack.
#define ROUND_STACK 8192
#define MIN_FREE (52 * 1024)
#define MIN_BLOCK (24 * 1024)

#define NVS_NS "dash"

typedef enum { MKT_UNKNOWN, MKT_CLOSED, MKT_PRE, MKT_OPEN, MKT_AFTER } mkt_t;

static char s_key[KEY_MAX + 1];
static char s_syms[MAX_SYMBOLS][12];
static int s_n_syms;
static volatile bool s_running;
static volatile bool s_now;      // fetch at the next tick
static int64_t s_next_ms;        // next round due
static int64_t s_last_ok_ms;     // last round with at least one quote
static volatile int s_last_status;
static int s_heap_skips;
static bool s_bad_key;

static int64_t now_ms(void) {
    return esp_timer_get_time() / 1000;
}

// ---- US market hours ----------------------------------------------------------

// Days since 1970-01-01 for a civil date (Howard Hinnant's algorithm).
static int64_t days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

// Day of month of the n-th Sunday of a month.
static int nth_sunday(int y, int m, int n) {
    int wday1 = (int)((days_from_civil(y, (unsigned)m, 1) + 4) % 7);  // 0 = Sun
    return 1 + (7 - wday1) % 7 + 7 * (n - 1);
}

// New York time is UTC-4 from the second Sunday of March (2:00 EST) to the
// first Sunday of November (2:00 EDT), UTC-5 otherwise.
static int eastern_offset_s(time_t utc) {
    struct tm g;
    gmtime_r(&utc, &g);
    int y = g.tm_year + 1900;
    int64_t start = days_from_civil(y, 3, (unsigned)nth_sunday(y, 3, 2)) * 86400
                    + 7 * 3600;
    int64_t end = days_from_civil(y, 11, (unsigned)nth_sunday(y, 11, 1)) * 86400
                  + 6 * 3600;
    return (utc >= start && utc < end) ? -4 * 3600 : -5 * 3600;
}

// Regular session 9:30-16:00 ET on weekdays, extended 4:00-20:00. Holidays
// are not known; on one the quotes simply don't change.
static mkt_t market_phase(void) {
    if (!dash_clock_valid()) return MKT_UNKNOWN;
    time_t utc = time(NULL);
    time_t et = utc + eastern_offset_s(utc);
    struct tm t;
    gmtime_r(&et, &t);
    if (t.tm_wday == 0 || t.tm_wday == 6) return MKT_CLOSED;
    int m = t.tm_hour * 60 + t.tm_min;
    if (m >= 9 * 60 + 30 && m < 16 * 60) return MKT_OPEN;
    if (m >= 4 * 60 && m < 9 * 60 + 30) return MKT_PRE;
    if (m >= 16 * 60 && m < 20 * 60) return MKT_AFTER;
    return MKT_CLOSED;
}

static const char *phase_label(mkt_t p) {
    switch (p) {
        case MKT_OPEN: return "Open";
        case MKT_PRE: return "Pre-market";
        case MKT_AFTER: return "After hours";
        case MKT_CLOSED: return "Closed";
        default: return "";
    }
}

static int interval_s(mkt_t p) {
    // Each round costs one call per symbol.
    int floor_s = (s_n_syms * 60 + CALLS_PER_MIN - 1) / CALLS_PER_MIN;
    int s;
    switch (p) {
        case MKT_OPEN: s = CONFIG_HOMEHUB_DASHBOARD_QUOTES_OPEN_S; break;
        case MKT_CLOSED: s = CLOSED_S; break;
        default: s = EXTENDED_S; break;
    }
    return s > floor_s ? s : floor_s;
}

// ---- configuration --------------------------------------------------------------

// "amd, nvda,SPY" -> AMD NVDA SPY. False if a symbol is malformed.
static bool parse_symbols(const char *in, char out[][12], int *n) {
    *n = 0;
    const char *p = in;
    while (*p) {
        while (*p == ',' || *p == ' ') p++;
        if (!*p) break;
        if (*n >= MAX_SYMBOLS) return false;
        int len = 0;
        while (*p && *p != ',' && *p != ' ') {
            char c = (char)toupper((unsigned char)*p++);
            if (!(isalnum((unsigned char)c) || c == '.' || c == '-') || len >= 11) {
                return false;
            }
            out[*n][len++] = c;
        }
        out[*n][len] = '\0';
        (*n)++;
    }
    return *n > 0;
}

static void load_str(nvs_handle_t h, const char *k, char *dst, size_t n) {
    size_t len = n;
    if (nvs_get_str(h, k, dst, &len) != ESP_OK) dst[0] = '\0';
}

void dash_quotes_init(void) {
    char key[KEY_MAX + 1] = CONFIG_HOMEHUB_DASHBOARD_QUOTES_KEY;
    char syms[SYMS_MAX + 1] = CONFIG_HOMEHUB_DASHBOARD_QUOTES_SYMBOLS;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        char k[KEY_MAX + 1], v[SYMS_MAX + 1];
        load_str(h, "qkey", k, sizeof(k));
        if (k[0]) memcpy(key, k, sizeof(key));
        load_str(h, "qsyms", v, sizeof(v));
        if (v[0]) memcpy(syms, v, sizeof(syms));
        nvs_close(h);
    }
    snprintf(s_key, sizeof(s_key), "%s", key);
    if (!parse_symbols(syms, s_syms, &s_n_syms)) s_n_syms = 0;
    s_now = true;
    ESP_LOGI(TAG, "%d symbols, API key %s", s_n_syms, s_key[0] ? "set" : "missing");
}

bool dash_quotes_configure(const char *key, const char *symbols,
                           const char **err) {
    char syms[MAX_SYMBOLS][12];
    int n = 0;
    if (symbols && !parse_symbols(symbols, syms, &n)) {
        *err = "symbols: up to 8, comma-separated, letters/digits/./-";
        return false;
    }
    if (key && strlen(key) > KEY_MAX) {
        *err = "key too long";
        return false;
    }
    nvs_handle_t h;
    bool nvs = nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK;
    if (key) {
        snprintf(s_key, sizeof(s_key), "%s", key);
        s_bad_key = false;
        if (nvs) nvs_set_str(h, "qkey", key);
    }
    if (symbols) {
        memcpy(s_syms, syms, sizeof(syms));
        s_n_syms = n;
        if (nvs) nvs_set_str(h, "qsyms", symbols);
    }
    if (nvs) {
        nvs_commit(h);
        nvs_close(h);
    }
    s_now = true;
    return true;
}

// ---- fetching -------------------------------------------------------------------

typedef struct {
    char buf[512];
    int len;
} rx_t;

static esp_err_t on_http(esp_http_client_event_t *e) {
    rx_t *rx = e->user_data;
    if (e->event_id == HTTP_EVENT_ON_DATA && rx
        && rx->len + e->data_len < (int)sizeof(rx->buf)) {
        memcpy(rx->buf + rx->len, e->data, (size_t)e->data_len);
        rx->len += e->data_len;
    }
    return ESP_OK;
}

static double num(const cJSON *o, const char *k) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? v->valuedouble : 0;
}

static void round_task(void *arg) {
    (void)arg;
    rx_t *rx = calloc(1, sizeof(*rx));
    char *url = malloc(URL_MAX);
    cJSON *doc = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(doc, "stocks");
    esp_http_client_handle_t c = NULL;
    const char *market = phase_label(market_phase());
    int got = 0;
    if (rx && url && doc && arr) {
        snprintf(url, URL_MAX, URL_FMT, s_syms[0], s_key);
        esp_http_client_config_t cfg = {
            .url = url,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .keep_alive_enable = true,  // one TLS handshake per round
            .timeout_ms = 8000,
            .buffer_size = 1024,
            .buffer_size_tx = 512,
            .event_handler = on_http,
            .user_data = rx,
        };
        c = esp_http_client_init(&cfg);
    }
    for (int i = 0; c && i < s_n_syms; i++) {
        snprintf(url, URL_MAX, URL_FMT, s_syms[i], s_key);
        esp_http_client_set_url(c, url);
        rx->len = 0;
        esp_err_t err = esp_http_client_perform(c);
        int status = err == ESP_OK ? esp_http_client_get_status_code(c) : -1;
        s_last_status = status;
        if (status == 401 || status == 403) {
            ESP_LOGW(TAG, "API key rejected (HTTP %d)", status);
            s_bad_key = true;
            break;
        }
        if (status == 429) {
            ESP_LOGW(TAG, "rate limited; backing off");
            s_next_ms = now_ms() + RATE_LIMIT_S * 1000;
            break;
        }
        if (status != 200) {
            ESP_LOGW(TAG, "%s: %s (HTTP %d)", s_syms[i], esp_err_to_name(err),
                     status);
            continue;
        }
        cJSON *q = cJSON_ParseWithLength(rx->buf, (size_t)rx->len);
        // {"c": price, "d": change, "dp": change %, "pc": prev close, "t": ts};
        // an unknown symbol comes back as all zeros.
        double price = num(q, "c");
        if (q && price > 0) {
            cJSON *s = cJSON_CreateObject();
            cJSON_AddStringToObject(s, "symbol", s_syms[i]);
            cJSON_AddNumberToObject(s, "price", price);
            cJSON_AddNumberToObject(s, "change", num(q, "d"));
            cJSON_AddNumberToObject(s, "changePct", num(q, "dp"));
            cJSON_AddStringToObject(s, "market", market);
            cJSON_AddItemToArray(arr, s);
            got++;
        }
        cJSON_Delete(q);
    }
    if (c) esp_http_client_cleanup(c);
    if (got) {
        double now = (double)time(NULL);
        cJSON_AddNumberToObject(doc, "stocks_updated", now);
        cJSON_AddNumberToObject(doc, "now", now);
        dash_store_set_direct(doc);
        dashboard_data_updated();
        s_last_ok_ms = now_ms();
        dash_store_set_direct_quotes(true);
    }
    cJSON_Delete(doc);
    free(url);
    free(rx);
    s_running = false;
    vTaskDelete(NULL);
}

void dash_quotes_tick(bool allowed) {
    int64_t now = now_ms();
    if (s_last_ok_ms && now - s_last_ok_ms > (int64_t)OWN_WINDOW_S * 1000) {
        dash_store_set_direct_quotes(false);
    }
    if (!allowed || s_running || !s_key[0] || s_bad_key || !s_n_syms) return;
    if (!s_now && now < s_next_ms) return;
    size_t free_b = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t block = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    if (free_b < MIN_FREE || block < MIN_BLOCK) {
        if (s_heap_skips++ % 20 == 0) {
            ESP_LOGW(TAG, "skipping: %u free, %u largest block", (unsigned)free_b,
                     (unsigned)block);
        }
        s_next_ms = now + RETRY_S * 1000;
        return;
    }
    s_now = false;
    s_next_ms = now + (int64_t)interval_s(market_phase()) * 1000;
    s_running = true;
    if (xTaskCreate(round_task, "quotes", ROUND_STACK, NULL, 3, NULL) != pdPASS) {
        s_running = false;
        s_next_ms = now + RETRY_S * 1000;
    }
}

void dash_quotes_status(char *buf, size_t n) {
    int64_t ago = s_last_ok_ms ? (now_ms() - s_last_ok_ms) / 1000 : -1;
    snprintf(buf, n,
             "quotes key=%s%s syms=%d market=%s last_ok=%llds http=%d heap_skips=%d",
             s_key[0] ? "set" : "missing", s_bad_key ? "(rejected)" : "",
             s_n_syms, phase_label(market_phase()), (long long)ago,
             s_last_status, s_heap_skips);
}

#else  // !CONFIG_HOMEHUB_DASHBOARD_QUOTES

void dash_quotes_init(void) {}

void dash_quotes_tick(bool allowed) {
    (void)allowed;
}

bool dash_quotes_configure(const char *key, const char *symbols,
                           const char **err) {
    (void)key;
    (void)symbols;
    *err = "built without CONFIG_HOMEHUB_DASHBOARD_QUOTES";
    return false;
}

void dash_quotes_status(char *buf, size_t n) {
    snprintf(buf, n, "quotes off (not in this build)");
    (void)TAG;
}

#endif
