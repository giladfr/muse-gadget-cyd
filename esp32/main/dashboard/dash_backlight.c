/*
 * Dashboard backlight implementation.
 */
#include "dash_backlight.h"

#include <stdio.h>

#include "dash_clock.h"
#include "driver/ledc.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

static const char *TAG = "dash.bl";

#define BL_GPIO 21
#define LDR_CHANNEL ADC_CHANNEL_6  // GPIO 34 on ADC1
#define BL_MODE LEDC_LOW_SPEED_MODE
#define BL_TIMER LEDC_TIMER_1
#define BL_CHANNEL LEDC_CHANNEL_4
#define BL_RES LEDC_TIMER_10_BIT
#define BL_MAX_DUTY ((1 << 10) - 1)
#define FADE_MS 400
#define LDR_PERIOD_MS 1000
// A touch on a screen dimmer than this only wakes it.
#define WAKE_ONLY_BELOW_PCT 20
// While touched recently, never below this.
#define ACTIVE_MIN_PCT 50

static bool s_ok;
static adc_oneshot_unit_handle_t s_adc;
static int s_ldr = -1;        // smoothed raw reading, -1 = none
static int64_t s_ldr_next_ms;
static int64_t s_last_touch_ms;
static int s_pct = -1;        // level last faded to
static int s_target = 100;

void dash_backlight_init(void) {
    ledc_timer_config_t t = {
        .speed_mode = BL_MODE,
        .duty_resolution = BL_RES,
        .timer_num = BL_TIMER,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_channel_config_t c = {
        .gpio_num = BL_GPIO,
        .speed_mode = BL_MODE,
        .channel = BL_CHANNEL,
        .timer_sel = BL_TIMER,
        .duty = BL_MAX_DUTY,  // led_status left it fully on
        .hpoint = 0,
    };
    if (ledc_timer_config(&t) != ESP_OK || ledc_channel_config(&c) != ESP_OK) {
        ESP_LOGW(TAG, "backlight PWM unavailable; staying fully on");
        return;
    }
    esp_err_t err = ledc_fade_func_install(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "fade unavailable: %s", esp_err_to_name(err));
    }
#if CONFIG_HOMEHUB_DASHBOARD_BL_AUTO
    adc_oneshot_unit_init_cfg_t u = {.unit_id = ADC_UNIT_1};
    adc_oneshot_chan_cfg_t ch = {.atten = ADC_ATTEN_DB_0,
                                 .bitwidth = ADC_BITWIDTH_DEFAULT};
    if (adc_oneshot_new_unit(&u, &s_adc) != ESP_OK
        || adc_oneshot_config_channel(s_adc, LDR_CHANNEL, &ch) != ESP_OK) {
        ESP_LOGW(TAG, "light sensor unavailable; no auto brightness");
        s_adc = NULL;
    }
#endif
    s_pct = 100;
    s_last_touch_ms = esp_timer_get_time() / 1000;
    s_ok = true;
    ESP_LOGI(TAG, "backlight PWM on GPIO %d%s", BL_GPIO,
             s_adc ? ", auto brightness from GPIO 34" : "");
}

static void read_ldr(int64_t now_ms) {
    if (!s_adc || now_ms < s_ldr_next_ms) return;
    s_ldr_next_ms = now_ms + LDR_PERIOD_MS;
    int raw;
    if (adc_oneshot_read(s_adc, LDR_CHANNEL, &raw) != ESP_OK) return;
    // Exponential smoothing: a shadow or a passing hand doesn't flicker it.
    s_ldr = s_ldr < 0 ? raw : (s_ldr * 3 + raw) / 4;
}

static int ambient_pct(void) {
#if CONFIG_HOMEHUB_DASHBOARD_BL_AUTO
    if (s_ldr < 0) return 100;
    const int bright = CONFIG_HOMEHUB_DASHBOARD_BL_LDR_BRIGHT;
    const int dark = CONFIG_HOMEHUB_DASHBOARD_BL_LDR_DARK;
    const int lo = CONFIG_HOMEHUB_DASHBOARD_BL_DARK_PCT;
    if (dark <= bright || s_ldr <= bright) return 100;
    if (s_ldr >= dark) return lo;
    return 100 - (100 - lo) * (s_ldr - bright) / (dark - bright);
#else
    return 100;
#endif
}

static bool is_night(void) {
    const int a = CONFIG_HOMEHUB_DASHBOARD_BL_NIGHT_START;
    const int b = CONFIG_HOMEHUB_DASHBOARD_BL_NIGHT_END;
    struct tm tm;
    if (a == b || !dash_clock_local(&tm)) return false;
    int h = tm.tm_hour;
    return a < b ? (h >= a && h < b) : (h >= a || h < b);
}

static int target_pct(int64_t now_ms) {
    int pct = ambient_pct();
    if (is_night() && pct > CONFIG_HOMEHUB_DASHBOARD_BL_NIGHT_PCT) {
        pct = CONFIG_HOMEHUB_DASHBOARD_BL_NIGHT_PCT;
    }
    int64_t idle_ms = now_ms - s_last_touch_ms;
    bool recent = idle_ms < 30 * 1000;
#if CONFIG_HOMEHUB_DASHBOARD_BL_IDLE_MIN > 0
    if (idle_ms >= (int64_t)CONFIG_HOMEHUB_DASHBOARD_BL_IDLE_MIN * 60 * 1000
        && pct > CONFIG_HOMEHUB_DASHBOARD_BL_IDLE_PCT) {
        pct = CONFIG_HOMEHUB_DASHBOARD_BL_IDLE_PCT;
    }
    recent = idle_ms < (int64_t)CONFIG_HOMEHUB_DASHBOARD_BL_IDLE_MIN * 60 * 1000;
#endif
    // Someone is looking: keep it readable even at night.
    if (recent && pct < ACTIVE_MIN_PCT) pct = ACTIVE_MIN_PCT;
    return pct;
}

static void fade_to(int pct) {
    // Perceived brightness is roughly quadratic in duty.
    uint32_t duty = (uint32_t)(BL_MAX_DUTY * pct * pct / 10000);
    if (ledc_set_fade_time_and_start(BL_MODE, BL_CHANNEL, duty, FADE_MS,
                                     LEDC_FADE_NO_WAIT) != ESP_OK) {
        ledc_set_duty(BL_MODE, BL_CHANNEL, duty);
        ledc_update_duty(BL_MODE, BL_CHANNEL);
    }
    s_pct = pct;
}

void dash_backlight_update(void) {
    if (!s_ok) return;
    int64_t now_ms = esp_timer_get_time() / 1000;
    read_ldr(now_ms);
    s_target = target_pct(now_ms);
    // Ignore jitter of a few percent from the sensor.
    if (s_target != s_pct && (s_target - s_pct > 3 || s_pct - s_target > 3
                              || s_target == 100 || s_target < 10)) {
        fade_to(s_target);
    }
}

bool dash_backlight_activity(void) {
    bool was_dim = s_ok && s_pct >= 0 && s_pct < WAKE_ONLY_BELOW_PCT;
    s_last_touch_ms = esp_timer_get_time() / 1000;
    dash_backlight_update();
    return was_dim;
}

void dash_backlight_debug(char *buf, size_t n) {
    snprintf(buf, n, "bl ok=%d ldr=%d target=%d%% level=%d%% night=%d", s_ok,
             s_ldr, s_target, s_pct, is_night());
}
