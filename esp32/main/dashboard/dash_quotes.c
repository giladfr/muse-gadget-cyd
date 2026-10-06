/*
 * Live stock quotes implementation: Nasdaq's public quote API, no key (the
 * same source as DeskPulse and bridge/bridge.py).
 */
#include "dash_quotes.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
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

// Nasdaq answers only requests that look like a browser's, and only for the
// right asset class (stocks, ETFs and indexes share the endpoints).
#define URL_FMT "https://api.nasdaq.com/api/quote/%s/%s?assetclass=%s"
#define USER_AGENT "Mozilla/5.0 (Macintosh; Intel Mac OS X) AppleWebKit/605.1.15 Safari/605.1.15"
static const char *const s_classes[] = {"stocks", "etf", "index"};
#define N_CLASSES 3

#define MAX_SYMBOLS DASH_MAX_STOCKS
#define SYMS_MAX 96
#define URL_MAX 160
// A quote response is ~2-3 KB of JSON.
#define BODY_MAX (8 * 1024)
// Intraday chart points kept while streaming (a full day is ~390 + extended).
#define CHART_MAX 1024
#define CHART_REFRESH_S (5 * 60)
#define EXTENDED_S 60
#define CLOSED_S (30 * 60)
#define RETRY_S 30
#define BLOCKED_S (5 * 60)
// A symbol no asset class answers for is probably mistyped: ask rarely.
#define UNKNOWN_SYMBOL_S (10 * 60)
// Be polite: at most one request per symbol every 2 s on average.
#define MIN_S_PER_SYMBOL 2
// While quotes keep arriving, the board owns them and pushed stocks are
// ignored; after this long without one, pushes are accepted again.
#define OWN_WINDOW_S (45 * 60)
// A TLS session needs a 16 KB receive buffer plus handshake state; the
// round's task an 8 KB stack, the body buffer 8 KB, the chart 4 KB.
#define ROUND_STACK 8192
#define MIN_FREE (64 * 1024)
#define MIN_BLOCK (24 * 1024)

#define NVS_NS "dash"

typedef enum { MKT_UNKNOWN, MKT_CLOSED, MKT_PRE, MKT_OPEN, MKT_AFTER } mkt_t;

typedef struct {
    char symbol[12];
    int8_t asset;                 // index into s_classes, -1 unknown
    float spark[DASH_HISTORY];    // today's chart, downsampled
    uint8_t spark_n;
    int64_t spark_ms;             // when the chart was fetched, 0 = never
    int64_t skip_until_ms;        // unknown to Nasdaq: don't ask until then
} sym_t;

static sym_t s_syms[MAX_SYMBOLS];
static int s_n_syms;
static volatile bool s_running;
static volatile bool s_now;      // fetch at the next tick
static int64_t s_next_ms;        // next round due
static int64_t s_last_ok_ms;     // last round with at least one quote
static volatile int s_last_status;
static volatile bool s_all_closed;  // Nasdaq said "Closed" for every symbol
static int s_heap_skips;

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
// come from Nasdaq's own marketStatus (s_all_closed).
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
    int floor_s = s_n_syms * MIN_S_PER_SYMBOL;
    int s;
    if (s_all_closed || p == MKT_CLOSED) {
        s = CLOSED_S;
    } else if (p == MKT_OPEN) {
        s = CONFIG_HOMEHUB_DASHBOARD_QUOTES_OPEN_S;
    } else {
        s = EXTENDED_S;
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

static void apply_symbols(char syms[][12], int n) {
    memset(s_syms, 0, sizeof(s_syms));
    for (int i = 0; i < n; i++) {
        memcpy(s_syms[i].symbol, syms[i], sizeof(s_syms[i].symbol));
        s_syms[i].asset = -1;
    }
    s_n_syms = n;
}

void dash_quotes_init(void) {
    char syms[SYMS_MAX + 1] = CONFIG_HOMEHUB_DASHBOARD_QUOTES_SYMBOLS;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        char v[SYMS_MAX + 1];
        size_t len = sizeof(v);
        if (nvs_get_str(h, "qsyms", v, &len) == ESP_OK && v[0]) {
            memcpy(syms, v, sizeof(syms));
        }
        nvs_close(h);
    }
    char parsed[MAX_SYMBOLS][12];
    int n = 0;
    if (parse_symbols(syms, parsed, &n)) apply_symbols(parsed, n);
    s_now = true;
    ESP_LOGI(TAG, "%d symbols from Nasdaq", s_n_syms);
}

bool dash_quotes_configure(const char *symbols, const char **err) {
    char parsed[MAX_SYMBOLS][12];
    int n = 0;
    if (!symbols || !parse_symbols(symbols, parsed, &n)) {
        *err = "symbols: up to 8, comma-separated, letters/digits/./-";
        return false;
    }
    if (s_running) {
        *err = "a refresh is running; try again in a few seconds";
        return false;
    }
    apply_symbols(parsed, n);
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "qsyms", symbols);
        nvs_commit(h);
        nvs_close(h);
    }
    s_now = true;
    return true;
}

// ---- fetching -------------------------------------------------------------------

// Response sink: either buffer the body (quotes) or stream-scan it for the
// chart's "y": prices, which don't fit in RAM as a whole document.
typedef struct {
    bool scan;
    char *buf;
    int len;
    bool overflow;
    // Chart scanner.
    float *pts;
    int n_pts;
    int match;       // characters of "\"y\":" matched so far
    char num[24];
    int num_len;
    bool in_num;
} rx_t;

static void scan_char(rx_t *rx, char c) {
    static const char pat[] = "\"y\":";
    if (rx->in_num) {
        if ((c >= '0' && c <= '9') || c == '.' || c == '-' || c == 'e' || c == 'E'
            || c == '+') {
            if (rx->num_len < (int)sizeof(rx->num) - 1) rx->num[rx->num_len++] = c;
            return;
        }
        if (c == ' ' && rx->num_len == 0) return;
        rx->num[rx->num_len] = '\0';
        if (rx->num_len && rx->n_pts < CHART_MAX) {
            rx->pts[rx->n_pts++] = strtof(rx->num, NULL);
        }
        rx->in_num = false;
        rx->num_len = 0;
    }
    if (c == pat[rx->match]) {
        if (++rx->match == (int)sizeof(pat) - 1) {
            rx->in_num = true;
            rx->match = 0;
        }
    } else {
        rx->match = c == pat[0] ? 1 : 0;
    }
}

static esp_err_t on_http(esp_http_client_event_t *e) {
    rx_t *rx = e->user_data;
    if (e->event_id != HTTP_EVENT_ON_DATA || !rx) return ESP_OK;
    const char *d = e->data;
    if (rx->scan) {
        for (int i = 0; i < e->data_len; i++) scan_char(rx, d[i]);
    } else if (rx->len + e->data_len < BODY_MAX) {
        memcpy(rx->buf + rx->len, d, (size_t)e->data_len);
        rx->len += e->data_len;
    } else {
        rx->overflow = true;
    }
    return ESP_OK;
}

// GET one Nasdaq endpoint; returns the HTTP status (-1 on transport error).
static int get(esp_http_client_handle_t c, rx_t *rx, char *url, const char *sym,
               const char *endpoint, int asset, bool scan) {
    snprintf(url, URL_MAX, URL_FMT, sym, endpoint, s_classes[asset]);
    esp_http_client_set_url(c, url);
    rx->scan = scan;
    rx->len = 0;
    rx->overflow = false;
    rx->n_pts = 0;
    rx->match = 0;
    rx->in_num = false;
    rx->num_len = 0;
    esp_err_t err = esp_http_client_perform(c);
    int status = err == ESP_OK ? esp_http_client_get_status_code(c) : -1;
    s_last_status = status;
    return status;
}

// "$631.75" / "-2.16" / "-0.34%" -> number; "UNCH" or "" -> dflt.
static double market_number(const cJSON *v, double dflt) {
    if (cJSON_IsNumber(v)) return v->valuedouble;
    if (!cJSON_IsString(v) || !v->valuestring) return dflt;
    char clean[32];
    int n = 0;
    for (const char *p = v->valuestring; *p && n < (int)sizeof(clean) - 1; p++) {
        if ((*p >= '0' && *p <= '9') || *p == '.' || *p == '-') clean[n++] = *p;
    }
    clean[n] = '\0';
    char *end;
    double d = strtod(clean, &end);
    return (n && end != clean) ? d : dflt;
}

#define NO_PRICE (-1.0)

// Parse a quote. Outside the regular session Nasdaq puts the extended-hours
// trade in secondaryData; show that when there is one, like DeskPulse.
static bool parse_quote(const char *body, int len, mkt_t phase, cJSON *out,
                        bool *closed) {
    cJSON *root = cJSON_ParseWithLength(body, (size_t)len);
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
    const cJSON *primary = cJSON_GetObjectItemCaseSensitive(data, "primaryData");
    const cJSON *secondary = cJSON_GetObjectItemCaseSensitive(data, "secondaryData");
    const cJSON *status = cJSON_GetObjectItemCaseSensitive(data, "marketStatus");
    const char *market = cJSON_IsString(status) ? status->valuestring : "";
    bool open = strcasecmp(market, "Open") == 0;
    bool extended = !open && cJSON_IsObject(secondary)
        && market_number(cJSON_GetObjectItemCaseSensitive(secondary, "lastSalePrice"),
                         NO_PRICE) > 0;
    const cJSON *shown = extended ? secondary : primary;
    double price = market_number(
        cJSON_GetObjectItemCaseSensitive(shown, "lastSalePrice"), NO_PRICE);
    bool ok = cJSON_IsObject(primary) && price > 0;
    if (ok) {
        cJSON_AddNumberToObject(out, "price", price);
        cJSON_AddNumberToObject(out, "change", market_number(
            cJSON_GetObjectItemCaseSensitive(shown, "netChange"), 0));
        cJSON_AddNumberToObject(out, "changePct", market_number(
            cJSON_GetObjectItemCaseSensitive(shown, "percentageChange"), 0));
        const char *label = extended && (phase == MKT_PRE || phase == MKT_AFTER)
                                ? phase_label(phase)
                                : (market[0] ? market : phase_label(phase));
        cJSON_AddStringToObject(out, "market", label);
        *closed = strcasecmp(market, "Closed") == 0;
    }
    cJSON_Delete(root);
    return ok;
}

// Downsample the streamed chart into the symbol's sparkline.
static void keep_spark(sym_t *s, const rx_t *rx) {
    if (rx->n_pts < 2) return;
    for (int i = 0; i < DASH_HISTORY; i++) {
        int k = (int)((int64_t)i * (rx->n_pts - 1) / (DASH_HISTORY - 1));
        s->spark[i] = rx->pts[k];
    }
    s->spark_n = DASH_HISTORY;
}

static void round_task(void *arg) {
    (void)arg;
    rx_t *rx = calloc(1, sizeof(*rx));
    char *url = malloc(URL_MAX);
    char *body = malloc(BODY_MAX);
    float *pts = malloc(sizeof(float) * CHART_MAX);
    cJSON *doc = cJSON_CreateObject();
    cJSON *arr = doc ? cJSON_AddArrayToObject(doc, "stocks") : NULL;
    esp_http_client_handle_t c = NULL;
    mkt_t phase = market_phase();
    int got = 0, closed_n = 0;
    bool blocked = false;
    if (rx && url && body && pts && arr) {
        rx->buf = body;
        rx->pts = pts;
        snprintf(url, URL_MAX, URL_FMT, s_syms[0].symbol, "info", s_classes[0]);
        esp_http_client_config_t cfg = {
            .url = url,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .keep_alive_enable = true,  // one TLS handshake per round
            .timeout_ms = 10000,
            .buffer_size = 2048,
            .buffer_size_tx = 1024,
            .user_agent = USER_AGENT,
            .event_handler = on_http,
            .user_data = rx,
        };
        c = esp_http_client_init(&cfg);
        if (c) {
            esp_http_client_set_header(c, "Accept", "application/json, text/plain, */*");
            esp_http_client_set_header(c, "Accept-Language", "en-US,en;q=0.9");
        }
    }
    int64_t now = now_ms();
    for (int i = 0; c && i < s_n_syms && !blocked; i++) {
        sym_t *s = &s_syms[i];
        if (now < s->skip_until_ms) continue;
        cJSON *q = cJSON_CreateObject();
        bool ok = false, closed = false;
        // The asset class that answered last time, else each in turn.
        for (int k = 0; q && k < N_CLASSES && !ok && !blocked; k++) {
            int a = s->asset >= 0 ? s->asset : k;
            if (s->asset >= 0 && k > 0) break;
            int st = get(c, rx, url, s->symbol, "info", a, false);
            if (st == 403 || st == 429) {
                ESP_LOGW(TAG, "Nasdaq refused (HTTP %d); backing off", st);
                blocked = true;
            } else if (st == 200 && !rx->overflow) {
                ok = parse_quote(body, rx->len, phase, q, &closed);
                if (ok) s->asset = (int8_t)a;
            }
        }
        if (!ok && !blocked) {
            if (s->asset >= 0) {
                s->asset = -1;  // try every class next time
            } else {
                ESP_LOGW(TAG, "%s: no quote from Nasdaq (unknown symbol?)", s->symbol);
                s->skip_until_ms = now + UNKNOWN_SYMBOL_S * 1000;
            }
        }
        if (ok) {
            // Today's chart for the sparkline, every few minutes.
            if (!s->spark_ms || now - s->spark_ms >= CHART_REFRESH_S * 1000) {
                if (get(c, rx, url, s->symbol, "chart", s->asset, true) == 200) {
                    keep_spark(s, rx);
                }
                s->spark_ms = now;
            }
            cJSON_AddStringToObject(q, "symbol", s->symbol);
            if (s->spark_n >= 2) {
                cJSON_AddItemToObject(q, "spark",
                                      cJSON_CreateFloatArray(s->spark, s->spark_n));
            }
            cJSON_AddItemToArray(arr, q);
            got++;
            closed_n += closed;
        } else {
            cJSON_Delete(q);
        }
    }
    if (c) esp_http_client_cleanup(c);
    if (blocked) s_next_ms = now_ms() + BLOCKED_S * 1000;
    if (got) {
        s_all_closed = closed_n == got;
        double t = (double)time(NULL);
        cJSON_AddNumberToObject(doc, "stocks_updated", t);
        cJSON_AddNumberToObject(doc, "now", t);
        dash_store_set_direct(doc);
        dashboard_data_updated();
        s_last_ok_ms = now_ms();
        dash_store_set_direct_quotes(true);
    }
    cJSON_Delete(doc);
    free(pts);
    free(body);
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
    if (!allowed || s_running || !s_n_syms) return;
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
             "quotes nasdaq syms=%d market=%s%s last_ok=%llds http=%d heap_skips=%d",
             s_n_syms, phase_label(market_phase()),
             s_all_closed ? "(closed)" : "", (long long)ago, s_last_status,
             s_heap_skips);
}

#else  // !CONFIG_HOMEHUB_DASHBOARD_QUOTES

void dash_quotes_init(void) {}

void dash_quotes_tick(bool allowed) {
    (void)allowed;
}

bool dash_quotes_configure(const char *symbols, const char **err) {
    (void)symbols;
    *err = "built without CONFIG_HOMEHUB_DASHBOARD_QUOTES";
    return false;
}

void dash_quotes_status(char *buf, size_t n) {
    snprintf(buf, n, "quotes off (not in this build)");
    (void)TAG;
}

#endif
