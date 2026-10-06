/*
 * Bridge poll implementation.
 */
#include "dash_net.h"

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "dash_store.h"
#include "dashboard.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "dash.net";

#define POLL_INTERVAL_MS (60 * 1000)
// The bridge document is ~1.5 KB; anything much bigger is not ours.
#define MAX_BODY (4 * 1024)

static TaskHandle_t s_task;

static void poll_once(void) {
    esp_http_client_config_t cfg = {
        .url = CONFIG_HOMEHUB_DASHBOARD_BRIDGE_URL,
        .timeout_ms = 5000,
        .buffer_size = 512,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return;
    char *buf = NULL;
    int len = 0;
    int status = -1;
    if (esp_http_client_open(c, 0) == ESP_OK) {
        int64_t clen = esp_http_client_fetch_headers(c);
        status = esp_http_client_get_status_code(c);
        if (status == 200 && clen <= MAX_BODY) {
            // Content-Length when the bridge sends it, else the cap.
            int cap = clen > 0 ? (int)clen : MAX_BODY;
            buf = malloc((size_t)cap + 1);
            while (buf && len < cap) {
                int n = esp_http_client_read(c, buf + len, cap - len);
                if (n <= 0) break;
                len += n;
            }
        }
        esp_http_client_close(c);
    }
    esp_http_client_cleanup(c);

    bool ok = false;
    if (buf && len > 0) {
        cJSON *root = cJSON_ParseWithLength(buf, (size_t)len);
        if (root) {
            ok = dash_store_set_bridge(root);
            cJSON_Delete(root);
        }
    }
    free(buf);
    if (ok) {
        dashboard_data_updated();
    } else {
        ESP_LOGW(TAG, "bridge poll failed (HTTP %d, %d bytes)", status, len);
    }
}

static void poll_task(void *arg) {
    (void)arg;
    // First poll soon after boot so the screens fill in.
    vTaskDelay(pdMS_TO_TICKS(5000));
    for (;;) {
        poll_once();
        // Sleep until the next poll, or until dash_net_poll_now().
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(POLL_INTERVAL_MS));
    }
}

void dash_net_start(void) {
    if (s_task) return;
    xTaskCreate(poll_task, "dash_net", 4096, NULL, 3, &s_task);
}

void dash_net_poll_now(void) {
    if (s_task) xTaskNotifyGive(s_task);
}
