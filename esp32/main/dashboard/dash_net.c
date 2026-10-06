/*
 * Bridge poll implementation.
 */
#include "dash_net.h"

#include <string.h>

#include "dash_store.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "dash.net";

#define POLL_INTERVAL_MS (60 * 1000)
#define MAX_BODY (24 * 1024)

static TaskHandle_t s_task;
static volatile bool s_poll_now;

typedef struct {
    char *buf;
    size_t len;
} dl_t;

static esp_err_t on_data(esp_http_client_event_t *e) {
    dl_t *d = e->user_data;
    if (e->event_id == HTTP_EVENT_ON_DATA && d->len + e->data_len < MAX_BODY) {
        memcpy(d->buf + d->len, e->data, e->data_len);
        d->len += e->data_len;
    }
    return ESP_OK;
}

static void poll_once(void) {
    char *buf = malloc(MAX_BODY + 1);
    if (!buf) return;
    dl_t d = {.buf = buf, .len = 0};
    esp_http_client_config_t cfg = {
        .url = CONFIG_HOMEHUB_DASHBOARD_BRIDGE_URL,
        .event_handler = on_data,
        .user_data = &d,
        .timeout_ms = 8000,
        .buffer_size = 1024,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    bool ok = false;
    if (c) {
        if (esp_http_client_perform(c) == ESP_OK
            && esp_http_client_get_status_code(c) == 200) {
            buf[d.len] = '\0';
            ok = dash_store_set_bridge(buf, d.len);
        } else {
            ESP_LOGW(TAG, "bridge poll failed: %d",
                     c ? esp_http_client_get_status_code(c) : -1);
        }
        esp_http_client_cleanup(c);
    }
    free(buf);
    if (ok) {
        // Wake the dashboard to redraw.
        extern void dashboard_data_updated(void);
        dashboard_data_updated();
    }
}

static void poll_task(void *arg) {
    (void)arg;
    // First poll soon after boot so the screens fill in.
    vTaskDelay(pdMS_TO_TICKS(5000));
    for (;;) {
        poll_once();
        for (int waited = 0; waited < POLL_INTERVAL_MS / 500; waited++) {
            if (s_poll_now) {
                s_poll_now = false;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }
}

void dash_net_start(void) {
    if (s_task) return;
    xTaskCreate(poll_task, "dash_net", 4096, NULL, 3, &s_task);
}

void dash_net_poll_now(void) {
    s_poll_now = true;
}
