/*
 * Dashboard state machine and main task.
 *
 * The dashboard task is the only one that draws dashboard pixels. Other tasks
 * (command handlers, the image downloader, the bridge poller, OTA) change
 * state under s_lock and wake it with a task notification. Between events the
 * task sleeps: touches arrive through the XPT2046 pen-down interrupt, and a
 * slow tick refreshes the clock and the "updated" label.
 */
#include "dashboard.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "esp_app_desc.h"

#include "dash_backlight.h"
#include "dash_cards.h"
#include "dash_clock.h"
#include "dash_events.h"
#include "dash_draw.h"
#include "dash_net.h"
#include "dash_quotes.h"
#include "dash_screens.h"
#include "dash_store.h"
#include "dash_touch.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "led_status.h"
#include "sdkconfig.h"

static const char *TAG = "dash";

typedef enum {
    DASH_OFF,        // not paired; status screen owns the display
    DASH_ACTIVE,     // dashboard screens
    DASH_PENDING,    // an image is being drawn; the dashboard stays off it
    DASH_TAKEOVER,   // pushed image + X dismiss button
    DASH_CALIBRATE,  // touch calibration targets
    DASH_UPDATING,   // firmware update progress
} dash_state_t;

// How often the header (clock, age label, Wi-Fi) is re-checked; only rows
// that changed are repainted. The clock screen ticks every second, on the
// second, for its second hand.
#define HEADER_REFRESH_MS 5000
// Land this far past the second, so the frame reads the new second.
#define CLOCK_TICK_SLACK_MS 15
// Wake-up fallback while idle: covers a lost pen-down interrupt. Without the
// interrupt, poll fast enough to catch a tap.
#define IDLE_WAKE_MS 500
#define POLL_WAKE_MS 50
// Frame interval while something fades (price-change flash).
#define ANIM_WAKE_MS 80
// Slide transition between screens.
#define SLIDE_STEPS 8
// Calibration gives up after this long without a tap.
#define CALIBRATE_TIMEOUT_MS (30 * 1000)
// The new firmware counts as healthy once the dashboard has been up this long.
#define HEALTHY_AFTER_MS (30 * 1000)

// Guarded by s_lock (s_state is also read without it by the renderer, which
// only needs to notice that it changed).
static volatile dash_state_t s_state = DASH_OFF;
static int s_screen = DASH_SCREEN_STOCKS;  // or DASH_SCREEN_COUNT + card
static int s_slide = 0;          // pending screen change: -1 / +1
static bool s_paired = false;
static bool s_link = false;
static int64_t s_splash_until = 0;  // version splash until this now_ms()
static int64_t s_next_tick_ms = 0;  // next header/clock refresh (renderer only)
static bool s_full = false;      // repaint the whole screen
static bool s_changed = false;   // data changed: diff and repaint those rows
static bool s_restore = false;   // leaving an image: reset the panel first
static bool s_draw_x = false;    // takeover began: draw the X button
static bool s_cal_start = false; // start touch calibration
static int s_ota_pct = -1;       // firmware update progress, -1 = none
static bool s_ota_dirty = false;
static int64_t s_takeover_ms;    // when the takeover began
static int64_t s_first_frame_ms; // 0 until the first full frame is drawn
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

// Calibration targets, in screen pixels.
static const int s_cal_t[3][2] = {{30, 60}, {290, 60}, {30, 200}};
static int s_cal_raw[3][2];
static int s_cal_step;
static int64_t s_cal_ms;

static int64_t now_ms(void) {
    return esp_timer_get_time() / 1000;
}

static void wake(void) {
    if (s_task) xTaskNotifyGive(s_task);
}

static void set_state(dash_state_t st) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state = st;
    xSemaphoreGive(s_lock);
}

// ---- rendering --------------------------------------------------------------

// Paint screen rows [y0, y1) of the prepared frame. Returns false if it was
// cut short (the state left `expect`, or the panel refused a strip).
static bool render_rows(int y0, int y1, dash_state_t expect) {
    y0 -= y0 % DASH_STRIP_H;
    int cur = 0;
    bool in_flight = false;
    bool ok = true;
    for (int y = y0; y < y1; y += DASH_STRIP_H) {
        if (s_state != expect) {
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

// Slide the content area from the previous frame to the prepared one: dir +1
// moves it left (next screen comes in from the right), -1 moves it right.
// Header and footer switch at once. Falls back to a plain repaint without
// the scratch RAM.
static bool render_slide(int dir) {
    const size_t bytes = DASH_W * DASH_STRIP_H * sizeof(uint16_t);
    uint16_t *a = malloc(bytes), *b = malloc(bytes);
    if (!a || !b) {
        free(a);
        free(b);
        return render_rows(0, DASH_H, DASH_ACTIVE);
    }
    bool ok = render_rows(0, DASH_CONTENT_Y0, DASH_ACTIVE)
              && render_rows(DASH_CONTENT_Y1, DASH_H, DASH_ACTIVE);
    for (int step = 1; ok && step <= SLIDE_STEPS; step++) {
        // Ease out (cubic): fast start, gentle landing.
        float t = 1.0f - (float)step / SLIDE_STEPS;
        int off = (int)(DASH_W * (1.0f - t * t * t) + 0.5f);
        int cur = 0;
        bool in_flight = false;
        for (int y = DASH_CONTENT_Y0; y < DASH_CONTENT_Y1; y += DASH_STRIP_H) {
            if (s_state != DASH_ACTIVE) {
                ok = false;
                break;
            }
            int h = DASH_CONTENT_Y1 - y < DASH_STRIP_H ? DASH_CONTENT_Y1 - y
                                                       : DASH_STRIP_H;
            dash_screen_draw_strip_prev(a, y, h);
            dash_screen_draw_strip(b, y, h);
            uint16_t *out = s_strip[cur];
            for (int r = 0; r < h; r++) {
                const uint16_t *ra = a + r * DASH_W, *rb = b + r * DASH_W;
                uint16_t *ro = out + r * DASH_W;
                if (dir > 0) {
                    memcpy(ro, ra + off, (DASH_W - off) * sizeof(uint16_t));
                    memcpy(ro + DASH_W - off, rb, off * sizeof(uint16_t));
                } else {
                    memcpy(ro, rb + DASH_W - off, off * sizeof(uint16_t));
                    memcpy(ro + off, ra, (DASH_W - off) * sizeof(uint16_t));
                }
            }
            if (in_flight) dashboard_display_draw_wait();
            in_flight = dashboard_display_draw_start(0, y, DASH_W, h, out);
            if (!in_flight) {
                ok = false;
                break;
            }
            cur ^= 1;
        }
        if (in_flight) dashboard_display_draw_wait();
    }
    free(a);
    free(b);
    return ok;
}

// The X button: a dark disc with a white X, drawn row span by row span so
// the image shows around it.
static void draw_x_button(void) {
    const float c = X_SIZE / 2.0f - 0.5f, r = 16.0f;
    uint16_t *row = s_strip[0];
    for (int y = 0; y < X_SIZE; y++) {
        float dy = y - c;
        if (dy * dy > r * r) continue;
        int half = (int)sqrtf(r * r - dy * dy);
        int x0 = (int)c - half, x1 = (int)c + half + 1;
        for (int x = x0; x < x1; x++) {
            // Two 3px diagonals across the middle of the disc.
            float u = x - c, v = y - c;
            bool on = fabsf(u) < 7.5f && fabsf(v) < 7.5f
                      && (fabsf(u - v) < 2.2f || fabsf(u + v) < 2.2f);
            row[x - x0] = on ? DASH_TEXT : DASH_RGB(20, 26, 40);
        }
        dashboard_display_draw(X_X0 + x0, X_Y0 + y, x1 - x0, 1, row);
    }
}

static void wifi_status(dash_status_t *st) {
    st->link = s_link;
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        st->wifi_bars = -1;
    } else {
        st->wifi_bars = ap.rssi >= -60 ? 3 : ap.rssi >= -70 ? 2
                        : ap.rssi >= -80 ? 1 : 0;
    }
}

// ---- touch ------------------------------------------------------------------

static int step(int s, int d) {
    int total = dash_screen_total();
    return (s + total + d) % total;
}

// A tap on the current frame's card buttons or banner. True if it was one.
static bool handle_card_tap(dash_touch_t t) {
    int hit = dash_screen_hit(t.x, t.y);
    if (hit == DASH_HIT_BANNER) {
        dash_banner_t b;
        int card = -1;
        if (dash_screen_banner(&b) && b.card[0]) card = dash_cards_find(b.card);
        dash_banner_dismiss();
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (card >= 0) {
            int target = DASH_SCREEN_COUNT + card;
            s_slide = target > s_screen ? 1 : -1;
            s_screen = target;
        }
        s_changed = true;
        xSemaphoreGive(s_lock);
        return true;
    }
    if (hit < 0) return false;
    const char *id, *title;
    const dash_card_button_t *buttons;
    int n;
    if (!dash_screen_card(&id, &title, &buttons, &n) || hit >= n) return false;
    char cid[16];
    snprintf(cid, sizeof(cid), "%s", id);
    // Pressed look first: Muse's acknowledgement can arrive (on the session
    // task) before dash_events_button() returns, and must not be overwritten.
    dash_cards_set_pressed(cid, hit, "Sending...");
    const char *status = dash_events_button(cid, title, buttons[hit].id,
                                            buttons[hit].label, buttons[hit].say);
    if (strcmp(status, "Sending...") != 0) dash_cards_set_status(cid, status);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_changed = true;
    xSemaphoreGive(s_lock);
    return true;
}

static void show_calibration(const char *msg, const char *hint, bool target) {
    dash_screen_prepare_message("Touch calibration", msg, hint,
                                target ? s_cal_t[s_cal_step][0] : -1,
                                target ? s_cal_t[s_cal_step][1] : -1, -1);
    render_rows(0, DASH_H, DASH_CALIBRATE);
}

static void calibrate_begin(void) {
    s_cal_step = 0;
    s_cal_ms = now_ms();
    set_state(DASH_CALIBRATE);
    ESP_LOGI(TAG, "touch calibration started");
    show_calibration("Tap the centre of the target", "1 of 3", true);
}

static void calibrate_end(void) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state = DASH_ACTIVE;
    s_full = true;
    xSemaphoreGive(s_lock);
}

static void calibrate_tap(dash_touch_t t) {
    s_cal_raw[s_cal_step][0] = t.raw_x;
    s_cal_raw[s_cal_step][1] = t.raw_y;
    s_cal_ms = now_ms();
    if (++s_cal_step < 3) {
        char hint[24];
        snprintf(hint, sizeof(hint), "%d of 3", s_cal_step + 1);
        show_calibration("Tap the centre of the target", hint, true);
        return;
    }
    dash_touch_cal_t cal;
    if (!dash_touch_cal_compute(s_cal_t, s_cal_raw, &cal)) {
        ESP_LOGW(TAG, "calibration rejected; starting over");
        s_cal_step = 0;
        show_calibration("That didn't line up - again", "1 of 3", true);
        return;
    }
    dash_touch_set_cal(&cal);
    show_calibration("Touch calibrated", "Saved", false);
    vTaskDelay(pdMS_TO_TICKS(1200));
    calibrate_end();
}

static void handle_touch(dash_touch_t t) {
    // A touch on a dimmed screen only wakes it.
    if (dash_backlight_activity()) return;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    dash_state_t st = s_state;
    xSemaphoreGive(s_lock);

    if (st == DASH_CALIBRATE) {
        if (t.type == DASH_TOUCH_TAP) calibrate_tap(t);
        return;
    }
    if (st == DASH_TAKEOVER) {
        if (t.type == DASH_TOUCH_TAP && t.x >= X_X0 && t.y < X_Y0 + X_SIZE) {
            ESP_LOGI(TAG, "takeover dismissed by X");
            dashboard_takeover_end();
        }
        return;
    }
    if (st != DASH_ACTIVE) return;

    if (t.type == DASH_TOUCH_LONG) {
        calibrate_begin();
        return;
    }
    if (t.type == DASH_TOUCH_TAP && handle_card_tap(t)) return;
    int d = 0;
    if (t.type == DASH_TOUCH_SWIPE_LEFT) {
        d = 1;
    } else if (t.type == DASH_TOUCH_SWIPE_RIGHT) {
        d = -1;
    } else if (t.type == DASH_TOUCH_TAP && t.y >= DASH_CONTENT_Y1 - 10) {
        if (t.x < 80) d = -1;
        else if (t.x > DASH_W - 80) d = 1;
    }
    if (!d) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_screen = step(s_screen, d);
    s_slide = d;
    xSemaphoreGive(s_lock);
}

// ---- task -------------------------------------------------------------------

static void activate(void) {
    ESP_LOGI(TAG, "activating dashboard");
    dashboard_display_set_active(true);
    dash_touch_init(s_task);
    dash_backlight_init();
    dash_clock_start();
    dash_quotes_init();
#if CONFIG_HOMEHUB_DASHBOARD_BRIDGE_POLL
    dash_net_start();
#endif
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_state == DASH_OFF) s_state = DASH_ACTIVE;
    s_full = true;
    xSemaphoreGive(s_lock);
}

// Milliseconds until the wall clock's next whole second.
static int ms_to_next_second(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return 1000 - (int)(tv.tv_usec / 1000);
}

static TickType_t next_wait(bool active) {
    if (!active) return portMAX_DELAY;  // dashboard_set_paired() wakes us
    if (dash_touch_tracking()) return pdMS_TO_TICKS(DASH_TOUCH_FAST_MS);
    if (s_state == DASH_ACTIVE && dash_screen_animating()) {
        return pdMS_TO_TICKS(ANIM_WAKE_MS);
    }
    int64_t ms = dash_touch_irq_driven() ? IDLE_WAKE_MS : POLL_WAKE_MS;
    // Wake for the next refresh rather than up to IDLE_WAKE_MS after it, so
    // the second hand doesn't stutter or skip.
    if (s_state == DASH_ACTIVE) {
        int64_t due = s_next_tick_ms - now_ms();
        if (due < ms) ms = due > 0 ? due : 0;
    }
    return pdMS_TO_TICKS(ms) + 1;
}

static void dash_task(void *arg) {
    (void)arg;
    bool active = false;
    bool need_full = false;  // last frame was cut short

    for (;;) {
        ulTaskNotifyTake(pdTRUE, next_wait(active));

        if (!active) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            bool paired = s_paired;
            xSemaphoreGive(s_lock);
            if (!paired) continue;
            activate();
            active = true;
            // Show version splash for 2.5 seconds on boot.
            s_splash_until = now_ms() + 2500;
            s_next_tick_ms = s_splash_until;  // wake to end it
        }

        dash_backlight_update();
        dash_touch_t t = dash_touch_poll();
        if (t.type != DASH_TOUCH_NONE) handle_touch(t);

        xSemaphoreTake(s_lock, portMAX_DELAY);
        dash_state_t st = s_state;
        int sc = s_screen;
        int slide = s_slide;
        bool full = s_full, changed = s_changed, restore = s_restore;
        bool draw_x = s_draw_x, cal = s_cal_start, ota_dirty = s_ota_dirty;
        int ota_pct = s_ota_pct;
        if (st == DASH_ACTIVE) {
            s_full = s_changed = s_restore = s_cal_start = false;
            s_slide = 0;
        }
        if (st == DASH_TAKEOVER) s_draw_x = false;
        s_ota_dirty = false;
        xSemaphoreGive(s_lock);
        int64_t now = now_ms();
        if (dash_cards_tick()) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_changed = true;
            xSemaphoreGive(s_lock);
            changed = true;
        }
        // A removed card can't stay on screen.
        if (sc >= dash_screen_total()) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_screen = sc = DASH_SCREEN_STOCKS;
            xSemaphoreGive(s_lock);
            full = true;
        }
        // Live quotes: quiet while an image or firmware is downloading.
        dash_quotes_tick(st != DASH_PENDING && st != DASH_UPDATING);

        if (st == DASH_UPDATING) {
            if (ota_dirty) {
                char pct[24];
                snprintf(pct, sizeof(pct), "%d%%", ota_pct);
                dash_screen_prepare_message("Updating", "Installing new firmware",
                                            pct, -1, -1, ota_pct);
                render_rows(0, DASH_H, DASH_UPDATING);
                need_full = true;
            }
            continue;
        }
        if (st == DASH_CALIBRATE) {
            if (now - s_cal_ms > CALIBRATE_TIMEOUT_MS) {
                ESP_LOGW(TAG, "calibration timed out; keeping the old mapping");
                calibrate_end();
            }
            continue;
        }
        if (st == DASH_TAKEOVER) {
            if (draw_x) {
                draw_x_button();
                ESP_LOGI(TAG, "takeover mode: X to dismiss");
            }
#if CONFIG_HOMEHUB_DASHBOARD_TAKEOVER_TIMEOUT_MIN > 0
            if (now - s_takeover_ms
                > (int64_t)CONFIG_HOMEHUB_DASHBOARD_TAKEOVER_TIMEOUT_MIN * 60000) {
                ESP_LOGI(TAG, "takeover timed out");
                dashboard_takeover_end();
            }
#endif
            continue;
        }
        if (st != DASH_ACTIVE) continue;
        if (cal) {
            calibrate_begin();
            continue;
        }

        if (restore) {
            // Ends the panel's image mode; the full repaint below covers it.
            dashboard_display_set_active(true);
            full = true;
        }
        // The splash just ended: the first dashboard frame repaints it all.
        if (s_splash_until && now >= s_splash_until) {
            s_splash_until = 0;
            full = true;
        }
        bool tick = now >= s_next_tick_ms;
        bool anim = dash_screen_animating();
        if (!(full || need_full || changed || tick || slide || anim)) continue;
        if (tick || slide) {
            s_next_tick_ms = sc == DASH_SCREEN_CLOCK && dash_clock_valid()
                                 ? now + ms_to_next_second() + CLOCK_TICK_SLACK_MS
                                 : now + HEADER_REFRESH_MS;
        }

        dash_status_t status;
        wifi_status(&status);
        xSemaphoreTake(s_render_lock, portMAX_DELAY);
        int y0, y1;
        bool repaint = full || need_full;
        // Boot splash: show version for 2.5s after activation.
        if (s_splash_until) {
            const esp_app_desc_t *desc = esp_app_get_description();
            char ver[40];
            snprintf(ver, sizeof(ver), "v%s", desc->version);
            dash_screen_prepare_message("Muse Dashboard", ver, "Starting...",
                                        -1, -1, -1);
            y0 = 0; y1 = DASH_H;
        } else {
            dash_screen_prepare(sc, &status, repaint || slide, &y0, &y1);
        }
        if (slide && !repaint) {
            need_full = !render_slide(slide);
        } else if (y0 < y1) {
            need_full = !render_rows(y0, y1, DASH_ACTIVE);
        }
        xSemaphoreGive(s_render_lock);

        if (!s_first_frame_ms && !need_full && y1 - y0 == DASH_H) {
            s_first_frame_ms = now_ms();
            ESP_LOGI(TAG, "first frame drawn, stack %u bytes free",
                     (unsigned)uxTaskGetStackHighWaterMark(NULL));
        }
    }
}

// ---- public API -----------------------------------------------------------

void dashboard_init(void) {
    ESP_LOGI(TAG, "dashboard init");
#ifdef CONFIG_HOMEHUB_SDCARD
    extern bool sd_card_init(void);
    sd_card_init();
#endif
    s_lock = xSemaphoreCreateMutex();
    s_render_lock = xSemaphoreCreateMutex();
    if (!s_lock || !s_render_lock) {
        ESP_LOGE(TAG, "no RAM for dashboard locks");
        return;
    }
    // Defer strip buffers and task until paired: BLE + WiFi + TLS during
    // pairing need the DMA RAM. dashboard_set_paired(true) activates.
    ESP_LOGI(TAG, "dashboard init deferred, waiting for pairing");
}

// Allocate strip buffers and start the render task. Called once on pairing.
static bool dashboard_activate(void) {
    if (s_task) return true;  // already active
    for (int i = 0; i < 2; i++) {
        s_strip[i] = heap_caps_malloc(DASH_W * DASH_STRIP_H * sizeof(uint16_t),
                                      MALLOC_CAP_DMA);
    }
    if (!s_strip[0] || !s_strip[1]) {
        ESP_LOGE(TAG, "no RAM for dashboard strip buffers");
        return false;
    }
    ESP_LOGI(TAG, "strip buffers ok, dashboard active");
    xTaskCreate(dash_task, "dashboard", 5120, NULL, 4, &s_task);
    return s_task != NULL;
}

void dashboard_set_paired(bool paired) {
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool was = s_paired;
    s_paired = paired;
    xSemaphoreGive(s_lock);
    if (paired && !was) {
        ESP_LOGI(TAG, "paired; dashboard will activate");
        if (dashboard_activate()) {
            wake();
        }
    }
}

void dashboard_set_link(bool up) {
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool was = s_link;
    s_link = up;
    if (was != up) s_changed = true;
    xSemaphoreGive(s_lock);
    if (was != up) wake();
}

bool dashboard_healthy(void) {
    // Nothing to vouch for when the dashboard never started.
    if (!s_task) return true;
    return s_first_frame_ms && now_ms() - s_first_frame_ms >= HEALTHY_AFTER_MS;
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
        s_takeover_ms = now_ms();
    }
    xSemaphoreGive(s_lock);
    if (ok) {
        dash_backlight_activity();  // a new picture is worth lighting up
        wake();
    }
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

void dashboard_calibrate(bool reset) {
    if (reset) {
        dash_touch_set_cal(NULL);
        return;
    }
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cal_start = true;
    xSemaphoreGive(s_lock);
    wake();
}

bool dashboard_quotes_configure(const char *symbols, const char **err) {
    bool ok = dash_quotes_configure(symbols, err);
    if (ok) wake();
    return ok;
}

void dashboard_quotes_status(char *buf, size_t n) {
    dash_quotes_status(buf, n);
}

void dashboard_ota_progress(int pct) {
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (pct < 0) {
        // Failed: back to the dashboard.
        if (s_state == DASH_UPDATING) {
            s_state = DASH_ACTIVE;
            s_full = true;
        }
        s_ota_pct = -1;
    } else if (s_state != DASH_OFF) {
        if (s_state != DASH_UPDATING) s_state = DASH_UPDATING;
        // Only repaint on whole steps: every repaint costs a full frame.
        if (pct / 2 != s_ota_pct / 2 || s_ota_pct < 0) s_ota_dirty = true;
        s_ota_pct = pct;
    }
    xSemaphoreGive(s_lock);
    wake();
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

bool dashboard_card(const cJSON *card, const char **err) {
    bool show = false;
    if (!s_lock || !dash_cards_set(card, &show, err)) {
        if (!s_lock) *err = "dashboard not running";
        return false;
    }
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(card, "id");
    int index = cJSON_IsString(id) ? dash_cards_find(id->valuestring) : -1;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (show && index >= 0 && s_state == DASH_ACTIVE) {
        int target = DASH_SCREEN_COUNT + index;
        if (target != s_screen) {
            s_slide = target > s_screen ? 1 : -1;
            s_screen = target;
        }
    }
    s_changed = true;
    xSemaphoreGive(s_lock);
    if (show) dash_backlight_activity();
    wake();
    return true;
}

bool dashboard_notify(const cJSON *n, const char **err) {
    if (!s_lock) {
        *err = "dashboard not running";
        return false;
    }
    if (!dash_banner_push(n, err)) return false;
    dash_backlight_activity();  // a notification is worth lighting up for
    mark_changed();
    return true;
}

cJSON *dashboard_events(bool clear) {
    return dash_events_take(clear);
}

void dashboard_debug(char *buf, size_t n) {
    char t[112], b[64], q[112], e[80];
    dash_touch_debug(t, sizeof(t));
    dash_backlight_debug(b, sizeof(b));
    dash_quotes_status(q, sizeof(q));
    dash_events_status(e, sizeof(e));
    snprintf(buf, n, "%s | %s | %s | %s | state=%d free=%u block=%u", t, b, q, e,
             (int)s_state,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}
