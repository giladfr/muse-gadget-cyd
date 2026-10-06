/*
 * Dashboard state machine and main task.
 *
 * The dashboard task is the only one that draws dashboard pixels. Other tasks
 * (command handlers, the image downloader, the bridge poller) change state
 * under s_lock and wake it with a task notification. Between events the task
 * sleeps: touches arrive through the XPT2046 pen-down interrupt, and a slow
 * tick refreshes the header's "updated" label.
 */
#include "dashboard.h"

#include <stdlib.h>
#include <string.h>

#include "dash_draw.h"
#include "dash_net.h"
#include "dash_screens.h"
#include "dash_store.h"
#include "dash_touch.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "led_status.h"

static const char *TAG = "dash";

typedef enum {
    DASH_OFF,       // not paired; status screen owns the display
    DASH_ACTIVE,    // dashboard screens
    DASH_PENDING,   // an image is being drawn; the dashboard stays off it
    DASH_TAKEOVER,  // pushed image + X dismiss button
} dash_state_t;

// How often the header's age label is re-checked (only changed rows redraw).
#define HEADER_REFRESH_MS (30 * 1000)
// Wake-up fallback while idle: covers a lost pen-down interrupt. Without the
// interrupt, poll fast enough to catch a tap.
#define IDLE_WAKE_MS 500
#define POLL_WAKE_MS 50

// Guarded by s_lock (s_state is also read without it by the renderer, which
// only needs to notice that it left DASH_ACTIVE).
static volatile dash_state_t s_state = DASH_OFF;
static dash_screen_t s_screen = DASH_SCREEN_STOCKS;
static bool s_paired = false;
static bool s_full = false;     // repaint the whole screen
static bool s_changed = false;  // data changed: diff and repaint those rows
static bool s_restore = false;  // leaving an image: reset the panel first
static bool s_draw_x = false;   // takeover began: draw the X button
static SemaphoreHandle_t s_lock;
// Held by the dashboard task while it renders; takeover_prepare() takes it
// to wait out a frame in progress.
static SemaphoreHandle_t s_render_lock;
static TaskHandle_t s_task;

// Two DMA-capable strips: one is filled while the other is on the wire.
static uint16_t *s_strip[2];

// X dismiss button (takeover mode).
#define X_SIZE 40
#define X_X0 (DASH_W - X_SIZE)
#define X_Y0 0

static void wake(void) {
    if (s_task) xTaskNotifyGive(s_task);
}

// Paint screen rows [y0, y1) of the prepared frame. Returns false if it was
// cut short (takeover started, or the panel refused a strip).
static bool render_rows(int y0, int y1) {
    y0 -= y0 % DASH_STRIP_H;
    int cur = 0;
    bool in_flight = false;
    bool ok = true;
    for (int y = y0; y < y1; y += DASH_STRIP_H) {
        if (s_state != DASH_ACTIVE) {
            ok = false;
            break;
        }
        int h = DASH_H - y < DASH_STRIP_H ? DASH_H - y : DASH_STRIP_H;
        // Fill this strip while the previous one is still being sent.
        dash_screen_draw_strip(s_strip[cur], y, h);
        if (in_flight) dashboard_display_draw_wait();
        in_flight = dashboard_display_draw_start(0, y, DASH_W, h, s_strip[cur]);
        if (!in_flight) {
            ESP_LOGW(TAG, "render: panel draw failed at y=%d", y);
            ok = false;
            break;
        }
        cur ^= 1;
    }
    if (in_flight) dashboard_display_draw_wait();
    return ok;
}

static void draw_x_button(void) {
    // Drawn over the pushed image: dark square + white X, in as many row
    // chunks as the strip buffer needs.
    const uint16_t bg = DASH_RGB(20, 20, 28);
    const int chunk = (DASH_W * DASH_STRIP_H) / X_SIZE;
    uint16_t *b = s_strip[0];
    for (int r0 = 0; r0 < X_SIZE; r0 += chunk) {
        int rows = X_SIZE - r0 < chunk ? X_SIZE - r0 : chunk;
        for (int r = 0; r < rows; r++) {
            int y = r0 + r;
            for (int x = 0; x < X_SIZE; x++) {
                // Two 5px-thick diagonals, 6px in from the edges.
                bool on = x >= 6 && x < X_SIZE - 6
                          && (abs(y - x) <= 2 || abs(y - (X_SIZE - 1 - x)) <= 2);
                b[r * X_SIZE + x] = on ? DASH_WHITE : bg;
            }
        }
        dashboard_display_draw(X_X0, X_Y0 + r0, X_SIZE, rows, b);
    }
}

static dash_screen_t step(dash_screen_t s, int d) {
    return (dash_screen_t)((s + DASH_SCREEN_COUNT + d) % DASH_SCREEN_COUNT);
}

static void handle_touch(dash_touch_t t) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    dash_state_t st = s_state;
    xSemaphoreGive(s_lock);

    if (st == DASH_TAKEOVER) {
        if (t.type == DASH_TOUCH_TAP && t.x >= X_X0 && t.y < X_Y0 + X_SIZE) {
            ESP_LOGI(TAG, "takeover dismissed by X");
            dashboard_takeover_end();
        }
        return;
    }
    if (st != DASH_ACTIVE) return;

    int d = 0;
    if (t.type == DASH_TOUCH_SWIPE_LEFT) {
        d = 1;
    } else if (t.type == DASH_TOUCH_SWIPE_RIGHT) {
        d = -1;
    } else if (t.type == DASH_TOUCH_TAP && t.y >= DASH_H - 28) {
        if (t.x < 70) d = -1;
        else if (t.x > DASH_W - 70) d = 1;
    }
    if (!d) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_screen = step(s_screen, d);
    s_full = true;
    xSemaphoreGive(s_lock);
}

static void activate(void) {
    ESP_LOGI(TAG, "activating dashboard");
    dashboard_display_set_active(true);
    dash_touch_init(s_task);
#if CONFIG_HOMEHUB_DASHBOARD_BRIDGE_POLL
    dash_net_start();
#endif
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_state == DASH_OFF) s_state = DASH_ACTIVE;
    s_full = true;
    xSemaphoreGive(s_lock);
}

static void dash_task(void *arg) {
    (void)arg;
    bool active = false;
    bool need_full = false;  // last frame was cut short
    bool stack_logged = false;
    int64_t next_header_ms = 0;

    for (;;) {
        TickType_t wait;
        if (!active) {
            wait = portMAX_DELAY;  // dashboard_set_paired() wakes us
        } else if (dash_touch_tracking()) {
            wait = pdMS_TO_TICKS(DASH_TOUCH_FAST_MS);
        } else {
            wait = pdMS_TO_TICKS(dash_touch_irq_driven() ? IDLE_WAKE_MS
                                                         : POLL_WAKE_MS);
        }
        ulTaskNotifyTake(pdTRUE, wait);

        if (!active) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            bool paired = s_paired;
            xSemaphoreGive(s_lock);
            if (!paired) continue;
            activate();
            active = true;
        }

        dash_touch_t t = dash_touch_poll();
        if (t.type != DASH_TOUCH_NONE) handle_touch(t);

        xSemaphoreTake(s_lock, portMAX_DELAY);
        dash_state_t st = s_state;
        dash_screen_t sc = s_screen;
        bool full = s_full, changed = s_changed;
        bool restore = s_restore, draw_x = s_draw_x;
        if (st == DASH_ACTIVE) {
            s_full = s_changed = s_restore = false;
        }
        if (st == DASH_TAKEOVER) s_draw_x = false;
        xSemaphoreGive(s_lock);

        if (st == DASH_TAKEOVER && draw_x) {
            draw_x_button();
            ESP_LOGI(TAG, "takeover mode: X to dismiss");
            continue;
        }
        if (st != DASH_ACTIVE) continue;

        if (restore) {
            // Ends the panel's image mode; the full repaint below covers it.
            dashboard_display_set_active(true);
            full = true;
        }
        int64_t now_ms = esp_timer_get_time() / 1000;
        bool tick = now_ms >= next_header_ms;
        if (!(full || need_full || changed || tick)) continue;
        if (tick) next_header_ms = now_ms + HEADER_REFRESH_MS;

        xSemaphoreTake(s_render_lock, portMAX_DELAY);
        int y0, y1;
        dash_screen_prepare(sc, full || need_full, &y0, &y1);
        if (y0 < y1) need_full = !render_rows(y0, y1);
        xSemaphoreGive(s_render_lock);

        if (!stack_logged && y1 - y0 == DASH_H) {
            stack_logged = true;
            ESP_LOGI(TAG, "first frame drawn, stack %u bytes free",
                     (unsigned)uxTaskGetStackHighWaterMark(NULL));
        }
    }
}

void dashboard_init(void) {
    ESP_LOGI(TAG, "dashboard init");
#ifdef CONFIG_HOMEHUB_SDCARD
    extern bool sd_card_init(void);
    sd_card_init();
#endif
    s_lock = xSemaphoreCreateMutex();
    s_render_lock = xSemaphoreCreateMutex();
    for (int i = 0; i < 2; i++) {
        s_strip[i] = heap_caps_malloc(DASH_W * DASH_STRIP_H * sizeof(uint16_t),
                                      MALLOC_CAP_DMA);
    }
    if (!s_lock || !s_render_lock || !s_strip[0] || !s_strip[1]) {
        ESP_LOGE(TAG, "no RAM for the dashboard (strip buffers / locks)");
        return;
    }
    ESP_LOGI(TAG, "strip buffers ok, waiting for pairing");
    xTaskCreate(dash_task, "dashboard", 3584, NULL, 4, &s_task);
}

void dashboard_set_paired(bool paired) {
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool was = s_paired;
    s_paired = paired;
    xSemaphoreGive(s_lock);
    if (paired && !was) {
        ESP_LOGI(TAG, "paired; dashboard will activate");
        wake();
    }
}

void dashboard_takeover_prepare(void) {
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool held = s_state == DASH_ACTIVE || s_state == DASH_TAKEOVER;
    if (held) s_state = DASH_PENDING;
    xSemaphoreGive(s_lock);
    if (!held) return;
    // A frame in progress notices the state change at its next strip; wait
    // for it so it cannot paint over the first rows of the image.
    xSemaphoreTake(s_render_lock, portMAX_DELAY);
    xSemaphoreGive(s_render_lock);
}

void dashboard_takeover_begin(void) {
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    // ACTIVE too, in case a caller skipped prepare().
    bool ok = s_state == DASH_PENDING || s_state == DASH_ACTIVE;
    if (ok) {
        s_state = DASH_TAKEOVER;
        s_draw_x = true;
    }
    xSemaphoreGive(s_lock);
    if (ok) wake();
}

void dashboard_takeover_end(void) {
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = s_state == DASH_TAKEOVER || s_state == DASH_PENDING;
    if (ok) {
        s_state = DASH_ACTIVE;
        s_draw_x = false;
        s_restore = true;
    }
    xSemaphoreGive(s_lock);
    if (!ok) return;
    wake();
    dash_net_poll_now();
}

static void mark_changed(void) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_changed = true;
    xSemaphoreGive(s_lock);
    wake();
}

bool dashboard_data_set(const char *screen, const cJSON *data) {
    if (!screen || !data || !s_lock) return false;
    bool ok;
    if (strcmp(screen, "calendar") == 0) {
        ok = dash_store_set_calendar(data);
    } else if (strcmp(screen, "bridge") == 0) {
        // Full bridge JSON: {"stocks": [...], "weather": {...}, ...}.
        // Pushed from the cloud; the HTTP poller is only a fallback.
        ok = dash_store_set_bridge(data);
    } else {
        ESP_LOGW(TAG, "dashboard_data_set: unknown screen '%s'", screen);
        return false;
    }
    // Rendering diffs against the last frame, so a change to another screen
    // costs one cheap prepare and no pixels.
    if (ok) mark_changed();
    return ok;
}

bool dashboard_data_set_json(const char *screen, const char *json) {
    if (!json) return false;
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        ESP_LOGW(TAG, "dashboard data for '%s': JSON parse failed",
                 screen ? screen : "?");
        return false;
    }
    bool ok = dashboard_data_set(screen, root);
    cJSON_Delete(root);
    return ok;
}

void dashboard_data_updated(void) {
    if (s_lock) mark_changed();
}

void dashboard_debug_touch(char *buf, size_t n) {
    dash_touch_debug(buf, n);
}
