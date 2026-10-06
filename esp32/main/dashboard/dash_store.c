/*
 * Dashboard data store implementation.
 */
#include "dash_store.h"

#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "dash.store";

static dash_store_t s_store;
static SemaphoreHandle_t s_lock;

static void ensure_lock(void) {
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
}

dash_store_t *dash_store_lock(void) {
    ensure_lock();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    return &s_store;
}

void dash_store_unlock(void) {
    xSemaphoreGive(s_lock);
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

bool dash_store_set_bridge(const char *json, size_t len) {
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) {
        ESP_LOGW(TAG, "bridge JSON parse failed");
        return false;
    }
    dash_store_t *s = dash_store_lock();

    const cJSON *stocks = cJSON_GetObjectItemCaseSensitive(root, "stocks");
    if (cJSON_IsArray(stocks)) {
        s->n_stocks = 0;
        const cJSON *it = NULL;
        cJSON_ArrayForEach(it, stocks) {
            if (s->n_stocks >= DASH_MAX_STOCKS) break;
            dash_stock_t *st = &s->stocks[s->n_stocks];
            copy_str(st->symbol, sizeof(st->symbol), it, "symbol");
            const cJSON *p = cJSON_GetObjectItemCaseSensitive(it, "price");
            const cJSON *c = cJSON_GetObjectItemCaseSensitive(it, "change");
            const cJSON *cp = cJSON_GetObjectItemCaseSensitive(it, "changePct");
            st->price = cJSON_IsNumber(p) ? p->valuedouble : 0;
            st->change = cJSON_IsNumber(c) ? c->valuedouble : 0;
            st->change_pct = cJSON_IsNumber(cp) ? cp->valuedouble : 0;
            copy_str(st->market, sizeof(st->market), it, "market");
            if (st->symbol[0]) s->n_stocks++;
        }
    }

    const cJSON *w = cJSON_GetObjectItemCaseSensitive(root, "weather");
    if (cJSON_IsObject(w)) {
        dash_weather_t *wx = &s->weather;
        const cJSON *t = cJSON_GetObjectItemCaseSensitive(w, "temp");
        const cJSON *f = cJSON_GetObjectItemCaseSensitive(w, "feels");
        const cJSON *h = cJSON_GetObjectItemCaseSensitive(w, "humidity");
        const cJSON *wd = cJSON_GetObjectItemCaseSensitive(w, "wind");
        wx->temp = cJSON_IsNumber(t) ? (int)t->valuedouble : 0;
        wx->feels = cJSON_IsNumber(f) ? (int)f->valuedouble : 0;
        wx->humidity = cJSON_IsNumber(h) ? (int)h->valuedouble : 0;
        wx->wind = cJSON_IsNumber(wd) ? (int)wd->valuedouble : 0;
        copy_str(wx->desc, sizeof(wx->desc), w, "desc");
        wx->forecast_n = 0;
        const cJSON *fc = cJSON_GetObjectItemCaseSensitive(w, "forecast");
        if (cJSON_IsArray(fc)) {
            const cJSON *it = NULL;
            cJSON_ArrayForEach(it, fc) {
                if (wx->forecast_n >= DASH_MAX_FORECAST) break;
                dash_forecast_t *d = &wx->forecast[wx->forecast_n];
                copy_str(d->day, sizeof(d->day), it, "day");
                const cJSON *hi = cJSON_GetObjectItemCaseSensitive(it, "high");
                const cJSON *lo = cJSON_GetObjectItemCaseSensitive(it, "low");
                const cJSON *co = cJSON_GetObjectItemCaseSensitive(it, "code");
                d->high = cJSON_IsNumber(hi) ? (int)hi->valuedouble : 0;
                d->low = cJSON_IsNumber(lo) ? (int)lo->valuedouble : 0;
                d->code = cJSON_IsNumber(co) ? (int)co->valuedouble : 0;
                wx->forecast_n++;
            }
        }
        s->weather_valid = true;
    }

    const cJSON *u = cJSON_GetObjectItemCaseSensitive(root, "updated");
    if (cJSON_IsNumber(u)) {
        s->updated_ts = (int64_t)u->valuedouble;
        s->updated_ticks = (int64_t)xTaskGetTickCount();
    }

    dash_store_unlock();
    cJSON_Delete(root);
    ESP_LOGI(TAG, "bridge data: %d stocks, weather %s", s->n_stocks,
             s->weather_valid ? "ok" : "missing");
    return true;
}

bool dash_store_set_calendar(const char *json, size_t len) {
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) {
        ESP_LOGW(TAG, "calendar JSON parse failed");
        return false;
    }
    dash_store_t *s = dash_store_lock();
    copy_str(s->events_label, sizeof(s->events_label), root, "label");
    s->n_events = 0;
    const cJSON *evs = cJSON_GetObjectItemCaseSensitive(root, "events");
    if (cJSON_IsArray(evs)) {
        const cJSON *it = NULL;
        cJSON_ArrayForEach(it, evs) {
            if (s->n_events >= DASH_MAX_EVENTS) break;
            dash_event_t *e = &s->events[s->n_events];
            copy_str(e->time, sizeof(e->time), it, "time");
            copy_str(e->title, sizeof(e->title), it, "title");
            if (e->title[0]) s->n_events++;
        }
    }
    dash_store_unlock();
    cJSON_Delete(root);
    ESP_LOGI(TAG, "calendar data: %d events", s->n_events);
    return true;
}

int64_t dash_store_age_s(void) {
    dash_store_t *s = dash_store_lock();
    int64_t age;
    if (s->updated_ts == 0) {
        age = INT64_MAX;
    } else {
        int64_t dt_ticks = (int64_t)xTaskGetTickCount() - s->updated_ticks;
        age = dt_ticks / configTICK_RATE_HZ;
    }
    dash_store_unlock();
    return age;
}

// America/Chicago is UTC-6 (CST) / UTC-5 (CDT). We approximate with a fixed
// -6h offset plus a crude DST guess; the label is informational only.
void dash_store_day_label(char *out, size_t out_len) {
    static const char *days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    dash_store_t *s = dash_store_lock();
    time_t t = (time_t)(s->updated_ts ? s->updated_ts - 6 * 3600 : 0);
    dash_store_unlock();
    if (!t) {
        strncpy(out, "---", out_len - 1);
        out[out_len - 1] = '\0';
        return;
    }
    struct tm tmv;
    gmtime_r(&t, &tmv);
    snprintf(out, out_len, "%s", days[tmv.tm_wday % 7]);
}
