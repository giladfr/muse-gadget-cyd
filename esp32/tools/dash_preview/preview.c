/*
 * Host preview of the CYD dashboard: runs the real dashboard code (task,
 * touch driver, renderer) against a fake panel and a simulated XPT2046, and
 * writes each interesting frame to raw/<name>.raw (320x240 RGB565, panel
 * byte order). make_previews.py turns them into PNGs and an animated GIF.
 * Build and run with run.sh.
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_http_client.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "esp_crt_bundle.h"
#include "noise_control.h"
#include "led_status.h"
#include "dashboard.h"

int64_t g_fake_us;
static int64_t g_t0_us;
static time_t g_fake_epoch;  // wall clock at start
static void tick_time(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    g_fake_us = (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000 - g_t0_us + 1000000;
}
time_t fake_time(time_t *t) { tick_time(); time_t v = g_fake_epoch + g_fake_us / 1000000; if (t) *t = v; return v; }

// ---- tasks
static pthread_mutex_t nm = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t nc = PTHREAD_COND_INITIALIZER;
static int notes;
typedef struct { TaskFunction_t f; void *a; } targ_t;
static void *trampoline(void *p) { targ_t *t = p; t->f(t->a); return NULL; }
BaseType_t xTaskCreate(TaskFunction_t f, const char *n, uint32_t st, void *a, UBaseType_t p, TaskHandle_t *h) {
    if (strcmp(n, "dashboard") != 0) { if (h) *h = (TaskHandle_t)2; return 1; }
    targ_t *t = malloc(sizeof *t); t->f = f; t->a = a;
    if (h) *h = (TaskHandle_t)1;
    pthread_t th; pthread_create(&th, NULL, trampoline, t);
    return 1;
}
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t t) {
    pthread_mutex_lock(&nm);
    if (!notes) {
        if (t == portMAX_DELAY) while (!notes) pthread_cond_wait(&nc, &nm);
        else {
            struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += (long)t * 1000000L; ts.tv_sec += ts.tv_nsec / 1000000000L; ts.tv_nsec %= 1000000000L;
            pthread_cond_timedwait(&nc, &nm, &ts);
        }
    }
    uint32_t n = notes; notes = 0;
    pthread_mutex_unlock(&nm);
    tick_time();
    return n;
}
BaseType_t xTaskNotifyGive(TaskHandle_t t) {
    if (t != (TaskHandle_t)1) return 1;
    pthread_mutex_lock(&nm); notes++; pthread_cond_signal(&nc); pthread_mutex_unlock(&nm); return 1;
}
void vTaskNotifyGiveFromISR(TaskHandle_t t, BaseType_t *w) { xTaskNotifyGive(t); }
void vTaskDelay(TickType_t t) { usleep(t * 1000); tick_time(); }
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t t) { return 9999; }
TickType_t xTaskGetTickCount(void) { return 0; }

// ---- fake panel + frame capture
static uint16_t fb[240][320];
static bool active;
static int frame_no;
static int capture = 0;          // capture each completed pass
static const char *capture_tag = "x";
static pthread_mutex_t fbm = PTHREAD_MUTEX_INITIALIZER;
static void snap(const char *name) {
    char path[256]; snprintf(path, sizeof path, "raw/%s.raw", name);
    pthread_mutex_lock(&fbm);
    FILE *f = fopen(path, "wb"); fwrite(fb, sizeof fb, 1, f); fclose(f);
    pthread_mutex_unlock(&fbm);
}
void dashboard_display_set_active(bool on) { if (on && !active) memset(fb, 0, sizeof fb); active = on; }
static int64_t s_last_snap_us;
// Flash capture: at most one snapshot per 70 ms (one per animation frame).
static bool last_flash_ms_ok(void) {
    tick_time();
    if (g_fake_us - s_last_snap_us < 70000) return false;
    s_last_snap_us = g_fake_us;
    return true;
}
static void blit(int x, int y, int w, int h, const uint16_t *p) {
    pthread_mutex_lock(&fbm);
    for (int r = 0; r < h; r++) memcpy(&fb[y + r][x], p + r * w, w * 2);
    pthread_mutex_unlock(&fbm);
    if (capture == 2 && y >= 32 && last_flash_ms_ok()) {
        char n[64]; snprintf(n, sizeof n, "%s_%03d", capture_tag, frame_no++); snap(n);
    } else if (capture == 1 && (y + h == 218 || y + h == 240)) {
        char n[64]; snprintf(n, sizeof n, "%s_%03d", capture_tag, frame_no++); snap(n);
    }
}
bool dashboard_display_draw(int x, int y, int w, int h, const uint16_t *p) { blit(x, y, w, h, p); return true; }
static const uint16_t *pend; static int px_, py_, pw_, ph_;
bool dashboard_display_draw_start(int x, int y, int w, int h, const uint16_t *p) { pend = p; px_ = x; py_ = y; pw_ = w; ph_ = h; return true; }
void dashboard_display_draw_wait(void) { blit(px_, py_, pw_, ph_, pend); }
bool sd_card_init(void) { return true; }

// ---- fake XPT2046
static volatile int pen_down; static volatile int raw_x = 2000, raw_y = 2000;
static int lv[64]; static int cs = 1, clk_rise, cmd, cur_val, miso;
esp_err_t gpio_config(const gpio_config_t *c) { return 0; }
static int value_for(int c) {
    switch (c & 0x70) {
        case 0x10: return pen_down ? raw_x : 0;
        case 0x50: return pen_down ? raw_y : 0;
        case 0x30: return pen_down ? 800 : 0;
        case 0x40: return pen_down ? 3000 : 4095;
    }
    return 0;
}
esp_err_t gpio_set_level(gpio_num_t p, uint32_t l) {
    if (p == 33) { if (!l && cs) clk_rise = 0; cs = l; }
    if (p == 32) lv[32] = l;
    if (p == 25 && !cs) {
        if (l && !lv[25]) { int n = clk_rise % 24; clk_rise++; if (n < 8) { cmd = (n == 0 ? 0 : cmd << 1) | lv[32]; if (n == 7) cur_val = value_for(cmd); } }
        else if (!l && lv[25]) { int n = (clk_rise - 1) % 24 + 1; miso = (n >= 9 && n <= 20) ? (cur_val >> (11 - (n - 9))) & 1 : 0; }
        lv[25] = l;
    }
    return 0;
}
int gpio_get_level(gpio_num_t p) { if (p == 39) return miso; if (p == 36) return pen_down ? 0 : 1; return 0; }
esp_err_t gpio_install_isr_service(int f) { return 0; }
esp_err_t gpio_set_intr_type(gpio_num_t p, gpio_int_type_t t) { return 0; }
static gpio_isr_t isr;
esp_err_t gpio_isr_handler_add(gpio_num_t p, gpio_isr_t h, void *a) { isr = h; return 0; }
static int intr_en;
esp_err_t gpio_intr_enable(gpio_num_t p) { intr_en = 1; return 0; }
esp_err_t gpio_intr_disable(gpio_num_t p) { intr_en = 0; return 0; }
// other stubs
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *c) { return NULL; }
esp_err_t esp_http_client_open(esp_http_client_handle_t c, int len) { return -1; }
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t c) { return 0; }
int esp_http_client_get_status_code(esp_http_client_handle_t c) { return 0; }
int esp_http_client_read(esp_http_client_handle_t c, char *b, int n) { return 0; }
esp_err_t esp_http_client_close(esp_http_client_handle_t c) { return 0; }
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t c) { return 0; }
esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *ap) { ap->rssi = -64; return 0; }
esp_err_t nvs_open(const char *ns, nvs_open_mode_t m, nvs_handle_t *h) { return -1; }
esp_err_t nvs_get_blob(nvs_handle_t h, const char *k, void *v, size_t *len) { return -1; }
esp_err_t nvs_set_blob(nvs_handle_t h, const char *k, const void *v, size_t len) { return -1; }
esp_err_t nvs_erase_key(nvs_handle_t h, const char *k) { return -1; }
esp_err_t nvs_commit(nvs_handle_t h) { return -1; }
void nvs_close(nvs_handle_t h) {}
esp_err_t esp_netif_sntp_init(const esp_sntp_config_t *c) { return 0; }
esp_err_t ledc_timer_config(const ledc_timer_config_t *c) { return 0; }
esp_err_t ledc_channel_config(const ledc_channel_config_t *c) { return 0; }
esp_err_t ledc_fade_func_install(int f) { return 0; }
esp_err_t ledc_set_fade_time_and_start(ledc_mode_t m, ledc_channel_t c, uint32_t d, uint32_t ms, ledc_fade_mode_t w) { return 0; }
esp_err_t ledc_set_duty(ledc_mode_t m, ledc_channel_t c, uint32_t d) { return 0; }
esp_err_t ledc_update_duty(ledc_mode_t m, ledc_channel_t c) { return 0; }
esp_err_t adc_oneshot_new_unit(const adc_oneshot_unit_init_cfg_t *c, adc_oneshot_unit_handle_t *h) { *h = (void *)1; return 0; }
esp_err_t adc_oneshot_config_channel(adc_oneshot_unit_handle_t h, adc_channel_t ch, const adc_oneshot_chan_cfg_t *c) { return 0; }
esp_err_t adc_oneshot_read(adc_oneshot_unit_handle_t h, adc_channel_t ch, int *out) { *out = 20; return 0; }

static void settle(int ms) { usleep(ms * 1000); }
static void press(int x, int y) { raw_x = x; raw_y = y; pen_down = 1; if (intr_en && isr) isr(NULL); }
static void release(void) { pen_down = 0; }
// Screen x -> raw for the default mapping (200..3700 / 240..3800).
static int rx(int x) { return 200 + x * 3500 / 320; }
static int ry(int y) { return 240 + y * 3560 / 240; }
static void tap(int x, int y) { press(rx(x), ry(y)); settle(40); release(); settle(60); }
static void swipe_left(void) { press(rx(260), ry(120)); settle(30); raw_x = rx(180); settle(30); raw_x = rx(60); settle(30); release(); }

static void push_stocks(const char *const *sym, int n, double base_shift, long now) {
    char buf[4096]; int o = 0;
    o += snprintf(buf + o, sizeof buf - o, "{\"stocks\":[");
    static const double price[] = {162.35, 181.20, 227.48, 517.03, 671.02, 589.66, 245.10, 412.77};
    static const double chg[] = {-2.16, 3.10, 1.05, -4.30, 0.48, 2.21, -6.85, 1.12};
    for (int i = 0; i < n; i++) {
        double p = price[i] + base_shift * (i % 3 == 0 ? -1 : 1) * (1 + i * 0.3);
        double c = chg[i] + base_shift * (i % 3 == 0 ? -1 : 1);
        o += snprintf(buf + o, sizeof buf - o, "%s{\"symbol\":\"%s\",\"price\":%.2f,\"change\":%.2f,\"changePct\":%.2f,\"market\":\"Open\"}",
                      i ? "," : "", sym[i], p, c, c / p * 100);
    }
    o += snprintf(buf + o, sizeof buf - o, "],\"stocks_updated\":%ld,\"now\":%ld}", now, now);
    dashboard_data_set_json("bridge", buf);
}

int main(void) {
    setenv("TZ", "CST6CDT,M3.2.0,M11.1.0", 1);
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    g_t0_us = (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
    g_fake_epoch = 1791301320;  // Tue 2026-10-06 10:42 CDT
    tick_time();
    dashboard_init();
    dashboard_set_link(true);
    dashboard_set_paired(true);
    settle(2600);
    snap("splash");
    settle(1200);  // the 3.4 s boot splash ends
    snap("stocks_empty");

    static const char *const sym[] = {"AMD", "NVDA", "AAPL", "MSFT", "SPY", "QQQ", "TSLA", "META"};
    // Build a little history for the sparklines.
    double walk[] = {0, .8, .3, 1.4, 1.1, 2.0, 1.6, 2.4, 2.9, 2.5, 3.3, 3.8, 3.1, 4.0, 4.6};
    for (int k = 0; k < 15; k++) push_stocks(sym, 5, walk[k] - 2, 1791301000 + k * 20);
    dashboard_data_set_json("calendar", "{\"label\":\"Tuesday, Oct 6\",\"events\":["
        "{\"time\":\"8:30 AM\",\"title\":\"Team standup\"},"
        "{\"time\":\"10:00 AM\",\"title\":\"Design review: CYD dashboard v2\"},"
        "{\"time\":\"12:30 PM\",\"title\":\"Lunch with Dana at the taco place on South Congress\"},"
        "{\"time\":\"5:45 PM\",\"title\":\"Pick up groceries\"}]}");
    dashboard_data_set_json("bridge", "{\"weather\":{\"location\":\"Austin, TX\",\"temp\":78,\"feels\":81,\"code\":2,\"desc\":\"Partly cloudy\",\"humidity\":55,\"wind\":8,\"forecast\":[{\"day\":\"Today\",\"high\":85,\"low\":66,\"code\":2},{\"day\":\"Wed\",\"high\":88,\"low\":69,\"code\":0},{\"day\":\"Thu\",\"high\":79,\"low\":64,\"code\":61},{\"day\":\"Fri\",\"high\":72,\"low\":58,\"code\":95}]},\"now\":1791301300}");
    settle(1800);
    snap("stocks");

    // Price tick: capture the flash fade.
    capture = 2; capture_tag = "flash"; frame_no = 0;
    push_stocks(sym, 5, 2.9, 1791301320);
    settle(1800);
    capture = 0;
    snap("stocks_after");

    // Swipe to weather: capture the slide.
    capture = 1; capture_tag = "slide"; frame_no = 0;
    swipe_left();
    settle(500);
    capture = 0;
    snap("weather");
    capture = 1; capture_tag = "slide2"; frame_no = 0;
    tap(300, 228);  // ">" zone
    settle(500);
    capture = 0;
    snap("calendar");

    // Busy day: compact calendar with "+N more".
    dashboard_data_set_json("calendar", "{\"label\":\"Wednesday, Oct 7\",\"events\":["
        "{\"time\":\"7:30 AM\",\"title\":\"Gym\"},{\"time\":\"9:00 AM\",\"title\":\"Standup\"},"
        "{\"time\":\"10:00 AM\",\"title\":\"1:1 with manager\"},{\"time\":\"11:30 AM\",\"title\":\"Roadmap planning\"},"
        "{\"time\":\"1:00 PM\",\"title\":\"Interview - firmware engineer\"},{\"time\":\"3:00 PM\",\"title\":\"Dentist\"},"
        "{\"time\":\"4:30 PM\",\"title\":\"School pickup\"},{\"time\":\"6:00 PM\",\"title\":\"Dinner\"},"
        "{\"time\":\"8:00 PM\",\"title\":\"Movie night\"},{\"time\":\"9:30 PM\",\"title\":\"Call parents\"}]}");
    settle(400);
    snap("calendar_compact");

    // 8 stocks: compact list.
    for (int k = 0; k < 15; k++) push_stocks(sym, 8, walk[k] - 2, 1791301400 + k * 20);
    tap(20, 228); settle(500);  // "<" back to weather
    tap(20, 228); settle(500);  // "<" to stocks
    snap("stocks_compact");

    // "<" from the first screen wraps to the clock (Tue 10:42:xx CDT).
    tap(20, 228); settle(500);
    snap("clock");
    tap(300, 228); settle(500);  // ">" back to stocks

    // Firmware update progress.
    dashboard_ota_progress(0); settle(150);
    dashboard_ota_progress(42); settle(300);
    snap("update");
    dashboard_ota_progress(-1); settle(400);

    // Calibration screen.
    dashboard_calibrate(false); settle(400);
    snap("calibrate");
    tap(30, 60); settle(300);
    snap("calibrate2");
    g_t0_us -= 31LL * 1000000;  // let it time out
    settle(800);

    // Muse's own cards: a question with buttons, then a status card.
    {
        cJSON *c = cJSON_Parse("{\"id\":\"groceries\",\"title\":\"Groceries\",\"sub\":\"Tuesday order\",\"tone\":\"accent\","
            "\"text\":\"Your usual order is ready: milk, eggs, sourdough, bananas and coffee beans. Place it for delivery tomorrow 9-11 AM?\","
            "\"rows\":[{\"label\":\"Total\",\"value\":\"$64.20\",\"detail\":\"5 items\"}],"
            "\"buttons\":[{\"id\":\"yes\",\"label\":\"Order\",\"say\":\"Yes, place the usual grocery order for tomorrow 9-11.\"},"
            "{\"id\":\"later\",\"label\":\"Later\"},{\"id\":\"no\",\"label\":\"Skip\",\"say\":\"Skip groceries this week.\"}],"
            "\"show\":true}");
        const char *err = "";
        if (!dashboard_card(c, &err)) printf("card failed: %s\n", err);
        cJSON_Delete(c);
        settle(600);
        snap("card_question");
        tap(52, 202);  // "Order"
        settle(400);
        snap("card_tapped");
        cJSON *ev = dashboard_events(false);
        char *evs = cJSON_PrintUnformatted(ev);
        printf("events: %s\n", evs ? evs : "?");
        cJSON_Delete(ev);
        free(evs);

        c = cJSON_Parse("{\"id\":\"day\",\"title\":\"Your day\",\"sub\":\"so far\",\"show\":true,\"rows\":["
            "{\"label\":\"Steps\",\"value\":\"6,240\",\"detail\":\"goal 10,000\",\"progress\":62,\"tone\":\"up\"},"
            "{\"label\":\"Focus time\",\"value\":\"2h 40m\",\"detail\":\"3 sessions\",\"progress\":67,\"tone\":\"blue\"},"
            "{\"label\":\"Inbox\",\"value\":\"14\",\"detail\":\"3 need a reply\",\"tone\":\"accent\",\"spark\":[22,19,25,30,18,16,14]},"
            "{\"label\":\"Build\",\"value\":\"failing\",\"detail\":\"main, 12 min ago\",\"tone\":\"down\"}]}");
        if (!dashboard_card(c, &err)) printf("card failed: %s\n", err);
        cJSON_Delete(c);
        settle(700);
        snap("card_rows");

        c = cJSON_Parse("{\"text\":\"Package delivered\",\"detail\":\"Front porch, 10:41 AM - from Amazon\",\"level\":\"success\"}");
        if (!dashboard_notify(c, &err)) printf("notify failed: %s\n", err);
        cJSON_Delete(c);
        settle(500);
        snap("banner");
        tap(160, 20);  // dismiss
        settle(400);
    }

    // Takeover: a fake photo + X.
    dashboard_takeover_prepare();
    pthread_mutex_lock(&fbm);
    for (int y = 0; y < 240; y++) for (int x = 0; x < 320; x++) {
        int r = 40 + x * 180 / 320, g = 80 + y * 120 / 240, b = 160 - x * 100 / 320;
        uint16_t v = ((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3);
        fb[y][x] = (uint16_t)((v >> 8) | (v << 8));
    }
    pthread_mutex_unlock(&fbm);
    dashboard_takeover_begin(); settle(300);
    snap("takeover");
    printf("ALL DONE\n");
    return 0;
}

// Stubs for the live-quotes module (its round task never runs here).
esp_err_t esp_http_client_set_url(esp_http_client_handle_t c, const char *url) { return 0; }
esp_err_t esp_http_client_perform(esp_http_client_handle_t c) { return -1; }
esp_err_t esp_http_client_set_header(esp_http_client_handle_t c, const char *k, const char *v) { return 0; }
esp_err_t esp_crt_bundle_attach(void *conf) { return 0; }
esp_err_t nvs_get_str(nvs_handle_t h, const char *k, char *v, size_t *len) { return -1; }
esp_err_t nvs_set_str(nvs_handle_t h, const char *k, const char *v) { return -1; }
void vTaskDelete(TaskHandle_t t) {}

// A fake Link session: chat sends are acknowledged at once.
static noise_ctrl_req_cb s_req_cb; static void *s_req_ctx;
static char s_sent_body[512];
bool noise_ctrl_is_connected(void) { return true; }
int64_t noise_ctrl_req_open(const char *verb, const char *path, const char *const *headers, bool end_body, noise_ctrl_req_cb cb, void *ctx) {
    s_req_cb = cb; s_req_ctx = ctx;
    printf("chat: %s %s\n", verb, path);
    return 42;
}
bool noise_ctrl_req_send(int64_t id, const void *data, size_t len, bool end_body, int wait_ms) {
    snprintf(s_sent_body, sizeof s_sent_body, "%.*s", (int)len, (const char *)data);
    printf("chat body: %s\n", s_sent_body);
    s_req_cb(s_req_ctx, 200, (const uint8_t *)"{\"id\":\"m1\"}", 11, true);
    return true;
}
void noise_ctrl_req_cancel(int64_t id) {}
