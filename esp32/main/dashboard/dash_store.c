/*
 * Dashboard data store implementation.
 */
#include "dash_store.h"

#include <string.h>

#include "dash_clock.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "dash.store";

static dash_store_t s_store;
static SemaphoreHandle_t s_lock;

static dash_store_t *store_lock(void) {
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    return &s_store;
}

static void store_unlock(void) {
    xSemaphoreGive(s_lock);
}

void dash_store_snapshot(dash_store_t *out) {
    dash_store_t *s = store_lock();
    *out = *s;
    store_unlock();
}

static void copy_str(char *dst, size_t n, const cJSON *obj, const char *key) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(v) && v->valuestring) {
        strncpy(dst, v->valuestring, n - 1);
        dst[n - 1] = '\0';
    } else {
        dst[0] = '\0';
    }
}

static double get_num(const cJSON *obj, const char *key, double dflt) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(v) ? v->valuedouble : dflt;
}

// Parse stocks into `out`; returns how many had a symbol.
static int parse_stocks(const cJSON *arr, dash_stock_t *out) {
    int n = 0;
    const cJSON *it = NULL;
    cJSON_ArrayForEach(it, arr) {
        if (n >= DASH_MAX_STOCKS) break;
        dash_stock_t *st = &out[n];
        memset(st, 0, sizeof(*st));
        copy_str(st->symbol, sizeof(st->symbol), it, "symbol");
        st->price = get_num(it, "price", 0);
        st->change = get_num(it, "change", 0);
        st->change_pct = get_num(it, "changePct", 0);
        copy_str(st->market, sizeof(st->market), it, "market");
        if (st->symbol[0]) n++;
    }
    return n;
}

// Carry each symbol's price history over from the previous quotes, append
// the new price, and note whether it moved. Caller holds the lock.
static void merge_history(dash_stock_t *fresh, int n, const dash_store_t *s,
                          int64_t now_us) {
    for (int i = 0; i < n; i++) {
        dash_stock_t *st = &fresh[i];
        const dash_stock_t *old = NULL;
        for (int j = 0; j < s->n_stocks; j++) {
            if (strcmp(s->stocks[j].symbol, st->symbol) == 0) {
                old = &s->stocks[j];
                break;
            }
        }
        if (old) {
            memcpy(st->history, old->history, sizeof(st->history));
            st->history_n = old->history_n;
            st->moved = old->moved;
            st->moved_us = old->moved_us;
            if (st->price != old->price) {
                st->moved = st->price > old->price ? 1 : -1;
                st->moved_us = now_us;
            }
        }
        if (st->history_n == DASH_HISTORY) {
            memmove(st->history, st->history + 1,
                    sizeof(st->history[0]) * (DASH_HISTORY - 1));
            st->history_n--;
        }
        st->history[st->history_n++] = (float)st->price;
    }
}

static void parse_weather(const cJSON *w, dash_weather_t *wx) {
    memset(wx, 0, sizeof(*wx));
    wx->temp = (int)get_num(w, "temp", 0);
    wx->feels = (int)get_num(w, "feels", 0);
    wx->humidity = (int)get_num(w, "humidity", 0);
    wx->wind = (int)get_num(w, "wind", 0);
    wx->code = (int)get_num(w, "code", -1);
    copy_str(wx->desc, sizeof(wx->desc), w, "desc");
    copy_str(wx->location, sizeof(wx->location), w, "location");
    const cJSON *fc = cJSON_GetObjectItemCaseSensitive(w, "forecast");
    const cJSON *it = NULL;
    cJSON_ArrayForEach(it, fc) {
        if (wx->forecast_n >= DASH_MAX_FORECAST) break;
        dash_forecast_t *d = &wx->forecast[wx->forecast_n];
        copy_str(d->day, sizeof(d->day), it, "day");
        d->high = (int)get_num(it, "high", 0);
        d->low = (int)get_num(it, "low", 0);
        d->code = (int)get_num(it, "code", 0);
        wx->forecast_n++;
    }
}

// Parse target for incoming quotes. Static (with the history it is ~1.5 KB,
// too much for a command handler's stack); only touched under the lock.
static dash_stock_t s_fresh[DASH_MAX_STOCKS];

bool dash_store_set_bridge(const cJSON *root) {
    if (!cJSON_IsObject(root)) {
        ESP_LOGW(TAG, "bridge data is not a JSON object");
        return false;
    }
    dash_weather_t wx;
    const cJSON *w = cJSON_GetObjectItemCaseSensitive(root, "weather");
    bool have_wx = cJSON_IsObject(w);
    if (have_wx) parse_weather(w, &wx);

    // Quote age at arrival: the bridge's clock (`now`, falling back to the
    // time the document was built) minus when the quotes were fetched
    // (`stocks_updated`, falling back to the combined `updated`).
    int64_t age_at_rx = 0;
    double fetched = get_num(root, "stocks_updated", get_num(root, "updated", 0));
    double now = get_num(root, "now", 0);
    if (fetched > 0 && now > fetched) age_at_rx = (int64_t)(now - fetched);
    // The board has no RTC: until SNTP answers, the bridge's clock will do.
    if (now > 0) dash_clock_seed((int64_t)now);

    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "stocks");
    int64_t now_us = esp_timer_get_time();
    dash_store_t *s = store_lock();
    int n_stocks = cJSON_IsArray(arr) ? parse_stocks(arr, s_fresh) : 0;
    if (n_stocks > 0) {
        merge_history(s_fresh, n_stocks, s, now_us);
        memcpy(s->stocks, s_fresh, sizeof(s_fresh[0]) * (size_t)n_stocks);
        s->n_stocks = n_stocks;
        s->stocks_age_at_rx_s = age_at_rx;
        s->stocks_rx_us = now_us ? now_us : 1;
    }
    if (have_wx) {
        s->weather = wx;
        s->weather_valid = true;
    }
    int total = s->n_stocks;
    bool wx_ok = s->weather_valid;
    store_unlock();

    ESP_LOGI(TAG, "bridge data: %d new stocks (%d shown, %llds old), weather %s",
             n_stocks, total, (long long)age_at_rx,
             have_wx ? "updated" : (wx_ok ? "kept" : "missing"));
    return true;
}

bool dash_store_set_calendar(const cJSON *root) {
    if (!cJSON_IsObject(root)) {
        ESP_LOGW(TAG, "calendar data is not a JSON object");
        return false;
    }
    struct tm tm;
    int day = dash_clock_local(&tm) ? tm.tm_year * 1000 + tm.tm_yday : -1;
    dash_store_t *s = store_lock();
    copy_str(s->events_label, sizeof(s->events_label), root, "label");
    s->n_events = 0;
    s->n_events_total = 0;
    s->events_day = day;
    const cJSON *evs = cJSON_GetObjectItemCaseSensitive(root, "events");
    const cJSON *it = NULL;
    cJSON_ArrayForEach(it, evs) {
        const cJSON *title = cJSON_GetObjectItemCaseSensitive(it, "title");
        if (!cJSON_IsString(title) || !title->valuestring[0]) continue;
        s->n_events_total++;
        if (s->n_events >= DASH_MAX_EVENTS) continue;
        dash_event_t *e = &s->events[s->n_events++];
        copy_str(e->time, sizeof(e->time), it, "time");
        copy_str(e->title, sizeof(e->title), it, "title");
    }
    int n = s->n_events_total;
    store_unlock();
    ESP_LOGI(TAG, "calendar data: %d events", n);
    return true;
}

int64_t dash_store_stocks_age_s(const dash_store_t *s) {
    if (s->stocks_rx_us == 0) return INT64_MAX;
    int64_t since_rx = (esp_timer_get_time() - s->stocks_rx_us) / 1000000;
    return s->stocks_age_at_rx_s + since_rx;
}
