/*
 * Dashboard state machine and main task.
 */
#include "dashboard.h"

#include <string.h>

#include "dash_draw.h"
#include "dash_net.h"
#include "dash_screens.h"
#include "dash_store.h"
#include "dash_touch.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "led_status.h"

static const char *TAG = "dash";

typedef enum {
    DASH_OFF,       // not paired; status screen owns the display
    DASH_ACTIVE,    // dashboard screens
    DASH_TAKEOVER,  // pushed image + X dismiss button
} dash_state_t;

static dash_state_t s_state = DASH_OFF;
static dash_screen_t s_screen = DASH_SCREEN_STOCKS;
static bool s_paired = false;
static bool s_redraw = false;
static SemaphoreHandle_t s_lock;
static uint16_t *s_strip;  // DMA-capable strip buffer

// X dismiss button (takeover mode).
#define X_SIZE 40
#define X_X0 (DASH_W - X_SIZE)
#define X_Y0 0

static void render_screen(dash_screen_t s) {
    if (!s_strip) {
        ESP_LOGE(TAG, "render: no strip buffer, skipping");
        return;
    }
    for (int y = 0; y < DASH_H; y += DASH_STRIP_H) {
        int h = DASH_H - y;
        if (h > DASH_STRIP_H) h = DASH_STRIP_H;
        dash_screen_draw_strip(s, s_strip, y, h);
        if (!dashboard_display_draw(0, y, DASH_W, h, s_strip)) {
            ESP_LOGW(TAG, "render: panel draw failed at y=%d", y);
            return;
        }
    }
}

static void draw_x_button(void) {
    // Drawn over the pushed image: dark rounded-ish square + white X.
    uint16_t *b = s_strip;
    int n = X_SIZE * X_SIZE;
    for (int i = 0; i < n; i++) b[i] = dash_rgb(20, 20, 28);
    // White X: two diagonals, 4px thick.
    for (int d = 6; d < X_SIZE - 6; d++) {
        for (int t = -2; t <= 2; t++) {
            int x1 = d, y1 = d + t;
            int x2 = d, y2 = (X_SIZE - 1 - d) + t;
            if (y1 >= 0 && y1 < X_SIZE) b[y1 * X_SIZE + x1] = DASH_WHITE;
            if (y2 >= 0 && y2 < X_SIZE) b[y2 * X_SIZE + x2] = DASH_WHITE;
        }
    }
    dashboard_display_draw(X_X0, X_Y0, X_SIZE, X_SIZE, b);
}

static void set_screen(dash_screen_t s) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_screen = s;
    s_redraw = true;
    xSemaphoreGive(s_lock);
}

static void handle_touch(dash_touch_t t) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    dash_state_t st = s_state;
    dash_screen_t sc = s_screen;
    xSemaphoreGive(s_lock);

    if (st == DASH_TAKEOVER) {
        if (t.type == DASH_TOUCH_TAP && t.x >= X_X0 && t.y < X_SIZE) {
            ESP_LOGI(TAG, "takeover dismissed by X");
            dashboard_takeover_end();
        }
        return;
    }
    if (st != DASH_ACTIVE) return;

    if (t.type == DASH_TOUCH_SWIPE_LEFT) {
        set_screen((dash_screen_t)((sc + 1) % DASH_SCREEN_COUNT));
    } else if (t.type == DASH_TOUCH_SWIPE_RIGHT) {
        set_screen((dash_screen_t)((sc + DASH_SCREEN_COUNT - 1) % DASH_SCREEN_COUNT));
    } else if (t.type == DASH_TOUCH_TAP && t.y >= DASH_H - 28) {
        if (t.x < 70) {
            set_screen((dash_screen_t)((sc + DASH_SCREEN_COUNT - 1) % DASH_SCREEN_COUNT));
        } else if (t.x > DASH_W - 70) {
            set_screen((dash_screen_t)((sc + 1) % DASH_SCREEN_COUNT));
        }
    }
}

static void dash_task(void *arg) {
    (void)arg;
    // Wait for pairing.
    while (!s_paired) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    ESP_LOGI(TAG, "activating dashboard");
    dashboard_display_set_active(true);
    dash_touch_init();
#if CONFIG_HOMEHUB_DASHBOARD_BRIDGE_POLL
    dash_net_start();
#endif

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state = DASH_ACTIVE;
    s_redraw = true;
    xSemaphoreGive(s_lock);

    for (;;) {
        dash_touch_t t = dash_touch_poll();
        if (t.type != DASH_TOUCH_NONE) handle_touch(t);

        bool redraw = false;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_redraw && s_state == DASH_ACTIVE) {
            s_redraw = false;
            redraw = true;
        }
        dash_screen_t sc = s_screen;
        xSemaphoreGive(s_lock);
        if (redraw) render_screen(sc);

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void dashboard_init(void) {
    ESP_LOGI(TAG, "dashboard init");
#ifdef CONFIG_HOMEHUB_SDCARD
    extern bool sd_card_init(void);
    sd_card_init();
#endif
    s_lock = xSemaphoreCreateMutex();
    s_strip = heap_caps_malloc(DASH_W * DASH_STRIP_H * sizeof(uint16_t),
                               MALLOC_CAP_DMA);
    if (!s_strip) {
        ESP_LOGE(TAG, "no DMA RAM for strip buffer");
        return;
    }
    ESP_LOGI(TAG, "strip buffer ok, waiting for pairing");
    xTaskCreate(dash_task, "dashboard", 3584, NULL, 4, NULL);
}

void dashboard_set_paired(bool paired) {
    if (paired && !s_paired) {
        ESP_LOGI(TAG, "paired; dashboard will activate");
    }
    s_paired = paired;
}

void dashboard_takeover_begin(void) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_state != DASH_ACTIVE) {
        xSemaphoreGive(s_lock);
        return;
    }
    s_state = DASH_TAKEOVER;
    xSemaphoreGive(s_lock);
    // The image is already on screen (drawn via led_status_draw_rect).
    draw_x_button();
    ESP_LOGI(TAG, "takeover mode: X to dismiss");
}

void dashboard_takeover_end(void) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_state != DASH_TAKEOVER) {
        xSemaphoreGive(s_lock);
        return;
    }
    // Re-activating clears the image and the screen; then redraw.
    xSemaphoreGive(s_lock);
    dashboard_display_set_active(true);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state = DASH_ACTIVE;
    dash_screen_t sc = s_screen;
    xSemaphoreGive(s_lock);
    render_screen(sc);
    dash_net_poll_now();
}

bool dashboard_data_set(const char *screen, const char *json) {
    if (!screen || !json) return false;
    if (strcmp(screen, "calendar") == 0) {
        if (!dash_store_set_calendar(json, strlen(json))) return false;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool vis = (s_state == DASH_ACTIVE && s_screen == DASH_SCREEN_CALENDAR);
        if (vis) s_redraw = true;
        xSemaphoreGive(s_lock);
        return true;
    }
    if (strcmp(screen, "bridge") == 0) {
        // Full bridge JSON: {"stocks": [...], "weather": {...}, "updated": ts}.
        // Pushed from the cloud; the HTTP poller is only a fallback.
        if (!dash_store_set_bridge(json, strlen(json))) return false;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool vis = (s_state == DASH_ACTIVE
                    && (s_screen == DASH_SCREEN_STOCKS
                        || s_screen == DASH_SCREEN_WEATHER));
        if (vis) s_redraw = true;
        xSemaphoreGive(s_lock);
        return true;
    }
    ESP_LOGW(TAG, "dashboard_data_set: unknown screen '%s'", screen);
    return false;
}

// Called by the net poller when fresh bridge data arrives.
void dashboard_data_updated(void) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_state == DASH_ACTIVE) s_redraw = true;
    xSemaphoreGive(s_lock);
}

void dashboard_debug_touch(char *buf, size_t n) {
    dash_touch_debug(buf, n);
}
