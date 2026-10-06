/*
 * XPT2046 bit-banged driver implementation.
 */
#include "dash_touch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "nvs.h"
#include "sdkconfig.h"

static const char *TAG = "dash.touch";

#define PIN_CLK  CONFIG_HOMEHUB_DASHBOARD_TOUCH_CLK
#define PIN_MOSI CONFIG_HOMEHUB_DASHBOARD_TOUCH_MOSI
#define PIN_MISO CONFIG_HOMEHUB_DASHBOARD_TOUCH_MISO
#define PIN_CS   CONFIG_HOMEHUB_DASHBOARD_TOUCH_CS
#define PIN_IRQ  CONFIG_HOMEHUB_DASHBOARD_TOUCH_IRQ

// Control bytes: start bit, channel, 12-bit, differential. PD=01 keeps the
// ADC on between the burst's conversions; PD=00 on the last one powers down
// and re-arms PENIRQ.
#define CMD_X     0x91  // screen X in landscape (panel Y+)
#define CMD_Y     0xD1  // screen Y in landscape (panel X+)
#define CMD_Z1    0xB1
#define CMD_Z2    0xC1
#define CMD_SLEEP 0x90

// Default raw 12-bit range of the 2432S028R panel (community calibration
// for the landscape orientation the status screen uses).
#define RAW_X_MIN 200
#define RAW_X_MAX 3700
#define RAW_Y_MIN 240
#define RAW_Y_MAX 3800
// Below this pressure the reading is noise (finger lifting or not there).
#define Z_THRESHOLD 400

#define SWIPE_MIN_DX 40
#define TAP_MAX_MS 400
#define TAP_MAX_MOVE 12
#define LONG_MAX_MOVE 30

#define NVS_NS "dash"
#define NVS_KEY "tcal"
#define CAL_VERSION 1

// Consecutive pen-up samples before a touch counts as released, so one bad
// sample mid-swipe doesn't split the gesture.
#define RELEASE_SAMPLES 2

static bool s_ok = false;
static bool s_irq_ok = false;
static TaskHandle_t s_notify;

static dash_touch_cal_t s_cal;

typedef struct {
    uint8_t version;
    dash_touch_cal_t cal;
} cal_blob_t;

// Gesture state (dashboard task only).
static bool s_down = false;
static bool s_long_fired = false;
static int s_down_rx, s_down_ry;
static int s_up_count = 0;
static int s_down_x, s_down_y;
static int64_t s_down_ms;
static int s_last_x, s_last_y;

// Last raw readings, for dash_touch_debug().
static volatile uint16_t s_raw_x, s_raw_y, s_raw_z;

static void irq_isr(void *arg) {
    (void)arg;
    BaseType_t woken = pdFALSE;
    if (s_notify) vTaskNotifyGiveFromISR(s_notify, &woken);
    portYIELD_FROM_ISR(woken);
}

// One 24-clock conversion: 8 command bits in, then the 12-bit result MSB
// first. DIN is latched on the rising edge; DOUT changes on the falling edge,
// the MSB after the 9th.
static uint16_t tp_xfer(uint8_t cmd) {
    for (int i = 7; i >= 0; i--) {
        gpio_set_level(PIN_MOSI, (cmd >> i) & 1);
        esp_rom_delay_us(1);
        gpio_set_level(PIN_CLK, 1);
        esp_rom_delay_us(1);
        gpio_set_level(PIN_CLK, 0);
    }
    gpio_set_level(PIN_MOSI, 0);
    uint16_t v = 0;
    for (int i = 0; i < 16; i++) {
        esp_rom_delay_us(1);
        gpio_set_level(PIN_CLK, 1);
        esp_rom_delay_us(1);
        gpio_set_level(PIN_CLK, 0);
        v = (uint16_t)((v << 1) | (gpio_get_level(PIN_MISO) & 1));
    }
    return (v >> 4) & 0xfff;
}

// Average of the two closest of three readings.
static int best_two_avg(int a, int b, int c) {
    int dab = abs(a - b), dac = abs(a - c), dbc = abs(b - c);
    if (dab <= dac && dab <= dbc) return (a + b) / 2;
    if (dac <= dab && dac <= dbc) return (a + c) / 2;
    return (b + c) / 2;
}

static int map_axis(int raw, int lo, int hi, int size) {
    int v = (raw - lo) * size / (hi - lo);
    if (v < 0) v = 0;
    if (v > size - 1) v = size - 1;
    return v;
}

// Read pressure and position. False when not pressed firmly enough.
static bool tp_sample(int *sx, int *sy) {
    gpio_set_level(PIN_CS, 0);
    int z1 = tp_xfer(CMD_Z1);
    int z2 = tp_xfer(CMD_Z2);
    int z = z1 + 4095 - z2;
    int x[3] = {0}, y[3] = {0};
    if (z >= Z_THRESHOLD) {
        tp_xfer(CMD_X);  // the first conversion after a mux change is noisy
        for (int i = 0; i < 3; i++) {
            x[i] = tp_xfer(CMD_X);
            y[i] = tp_xfer(CMD_Y);
        }
    }
    tp_xfer(CMD_SLEEP);
    gpio_set_level(PIN_CS, 1);

    s_raw_z = (uint16_t)(z < 0 ? 0 : z);
    if (z < Z_THRESHOLD) return false;
    int rx = best_two_avg(x[0], x[1], x[2]);
    int ry = best_two_avg(y[0], y[1], y[2]);
    s_raw_x = (uint16_t)rx;
    s_raw_y = (uint16_t)ry;

    int raw[2] = {rx, ry};
    *sx = map_axis(raw[s_cal.src_x & 1], s_cal.x_lo, s_cal.x_hi, 320);
    *sy = map_axis(raw[s_cal.src_y & 1], s_cal.y_lo, s_cal.y_hi, 240);
    return true;
}

static void default_cal(dash_touch_cal_t *c) {
    c->src_x = 0;
    c->src_y = 1;
    c->x_lo = RAW_X_MIN;
    c->x_hi = RAW_X_MAX;
    c->y_lo = RAW_Y_MIN;
    c->y_hi = RAW_Y_MAX;
#if CONFIG_HOMEHUB_DASHBOARD_TOUCH_SWAP_XY
    // Swapped panels: each raw axis drives the other screen axis.
    c->src_x = 1;
    c->src_y = 0;
    c->x_lo = RAW_Y_MIN;
    c->x_hi = RAW_Y_MAX;
    c->y_lo = RAW_X_MIN;
    c->y_hi = RAW_X_MAX;
#endif
#if CONFIG_HOMEHUB_DASHBOARD_TOUCH_INVERT_X
    int16_t t = c->x_lo; c->x_lo = c->x_hi; c->x_hi = t;
#endif
#if CONFIG_HOMEHUB_DASHBOARD_TOUCH_INVERT_Y
    int16_t u = c->y_lo; c->y_lo = c->y_hi; c->y_hi = u;
#endif
}

static bool cal_sane(const dash_touch_cal_t *c) {
    return c->src_x <= 1 && c->src_y <= 1 && c->src_x != c->src_y
        && abs(c->x_hi - c->x_lo) >= 500 && abs(c->y_hi - c->y_lo) >= 500;
}

static void load_cal(void) {
    default_cal(&s_cal);
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    cal_blob_t blob;
    size_t len = sizeof(blob);
    if (nvs_get_blob(h, NVS_KEY, &blob, &len) == ESP_OK && len == sizeof(blob)
        && blob.version == CAL_VERSION && cal_sane(&blob.cal)) {
        s_cal = blob.cal;
        ESP_LOGI(TAG, "touch calibration loaded from NVS");
    }
    nvs_close(h);
}

void dash_touch_get_cal(dash_touch_cal_t *out) {
    *out = s_cal;
}

void dash_touch_set_cal(const dash_touch_cal_t *cal) {
    nvs_handle_t h;
    bool nvs = nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK;
    if (cal && cal_sane(cal)) {
        s_cal = *cal;
        if (nvs) {
            cal_blob_t blob = {.version = CAL_VERSION, .cal = *cal};
            nvs_set_blob(h, NVS_KEY, &blob, sizeof(blob));
        }
    } else {
        default_cal(&s_cal);
        if (nvs) nvs_erase_key(h, NVS_KEY);
    }
    if (nvs) {
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "touch map: X from raw %c %d..%d, Y from raw %c %d..%d",
             s_cal.src_x ? 'y' : 'x', s_cal.x_lo, s_cal.x_hi,
             s_cal.src_y ? 'y' : 'x', s_cal.y_lo, s_cal.y_hi);
}

bool dash_touch_cal_compute(const int t[3][2], const int r[3][2],
                            dash_touch_cal_t *out) {
    // t[0] -> t[1] moves only along screen X; t[0] -> t[2] only along Y.
    // The raw axis that changed most is the one that drives that screen axis.
    int dxa = abs(r[1][0] - r[0][0]), dxb = abs(r[1][1] - r[0][1]);
    int dya = abs(r[2][0] - r[0][0]), dyb = abs(r[2][1] - r[0][1]);
    int sx = dxa >= dxb ? 0 : 1;
    int sy = dya >= dyb ? 0 : 1;
    int span_x = t[1][0] - t[0][0], span_y = t[2][1] - t[0][1];
    if (sx == sy || span_x <= 0 || span_y <= 0) return false;
    float kx = (float)(r[1][sx] - r[0][sx]) / (float)span_x;  // raw per px
    float ky = (float)(r[2][sy] - r[0][sy]) / (float)span_y;
    dash_touch_cal_t c = {
        .src_x = (uint8_t)sx,
        .src_y = (uint8_t)sy,
        .x_lo = (int16_t)(r[0][sx] - kx * t[0][0]),
        .x_hi = (int16_t)(r[0][sx] + kx * (320 - t[0][0])),
        .y_lo = (int16_t)(r[0][sy] - ky * t[0][1]),
        .y_hi = (int16_t)(r[0][sy] + ky * (240 - t[0][1])),
    };
    if (!cal_sane(&c)) return false;
    *out = c;
    return true;
}


bool dash_touch_init(TaskHandle_t notify) {
    s_notify = notify;
    load_cal();
    gpio_config_t out = {
        .pin_bit_mask = (1ULL << PIN_CLK) | (1ULL << PIN_MOSI) | (1ULL << PIN_CS),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config_t in = {
        // GPIO 34-39 are input-only with no internal pulls; the board pulls
        // PENIRQ up.
        .pin_bit_mask = (1ULL << PIN_MISO) | (1ULL << PIN_IRQ),
        .mode = GPIO_MODE_INPUT,
    };
    if (gpio_config(&out) != ESP_OK || gpio_config(&in) != ESP_OK) {
        ESP_LOGE(TAG, "touch gpio config failed");
        return false;
    }
    gpio_set_level(PIN_CS, 1);
    gpio_set_level(PIN_CLK, 0);

    // Leave the controller powered down with PENIRQ armed, then probe.
    gpio_set_level(PIN_CS, 0);
    tp_xfer(CMD_SLEEP);
    gpio_set_level(PIN_CS, 1);
    gpio_set_level(PIN_CS, 0);
    uint16_t z1 = tp_xfer(CMD_Z1);
    uint16_t x = tp_xfer(CMD_X);
    tp_xfer(CMD_SLEEP);
    gpio_set_level(PIN_CS, 1);
    ESP_LOGI(TAG, "XPT2046 bit-bang CLK=%d MOSI=%d MISO=%d CS=%d IRQ=%d "
             "(probe z1=0x%03x x=0x%03x)", PIN_CLK, PIN_MOSI, PIN_MISO, PIN_CS,
             PIN_IRQ, z1, x);

    esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "isr service: %s; touch falls back to polling",
                 esp_err_to_name(err));
    } else {
        s_irq_ok = gpio_set_intr_type(PIN_IRQ, GPIO_INTR_NEGEDGE) == ESP_OK
                   && gpio_isr_handler_add(PIN_IRQ, irq_isr, NULL) == ESP_OK
                   && gpio_intr_enable(PIN_IRQ) == ESP_OK;
        if (!s_irq_ok) ESP_LOGW(TAG, "pen-down interrupt unavailable; polling");
    }
    s_ok = true;
    return true;
}

bool dash_touch_irq_driven(void) {
    return s_irq_ok;
}

bool dash_touch_tracking(void) {
    return s_ok && (s_down || gpio_get_level(PIN_IRQ) == 0);
}

static dash_touch_t gesture_end(int64_t now) {
    dash_touch_t ev = {.type = DASH_TOUCH_NONE, .x = 0, .y = 0};
    int dx = s_last_x - s_down_x;
    int dy = s_last_y - s_down_y;
    int64_t dt = now - s_down_ms;
    if (dx < -SWIPE_MIN_DX && abs(dy) < SWIPE_MIN_DX) {
        ev.type = DASH_TOUCH_SWIPE_LEFT;
    } else if (dx > SWIPE_MIN_DX && abs(dy) < SWIPE_MIN_DX) {
        ev.type = DASH_TOUCH_SWIPE_RIGHT;
    } else if (dt < TAP_MAX_MS && abs(dx) < TAP_MAX_MOVE
               && abs(dy) < TAP_MAX_MOVE) {
        ev.type = DASH_TOUCH_TAP;
        ev.x = s_down_x;
        ev.y = s_down_y;
        ev.raw_x = s_down_rx;
        ev.raw_y = s_down_ry;
    }
    return ev;
}

dash_touch_t dash_touch_poll(void) {
    dash_touch_t ev = {.type = DASH_TOUCH_NONE, .x = 0, .y = 0};
    if (!s_ok) return ev;
    int64_t now = esp_timer_get_time() / 1000;
    int x, y;
    bool pressed = gpio_get_level(PIN_IRQ) == 0 && tp_sample(&x, &y);
    if (pressed) {
        s_up_count = 0;
        if (!s_down) {
            // Mask pen-down interrupts until the gesture ends: the line
            // chatters while the controller converts, and the task polls on
            // its own while a touch is tracked.
            if (s_irq_ok) gpio_intr_disable(PIN_IRQ);
            s_down = true;
            s_long_fired = false;
            s_down_x = s_last_x = x;
            s_down_y = s_last_y = y;
            s_down_rx = s_raw_x;
            s_down_ry = s_raw_y;
            s_down_ms = now;
        } else {
            s_last_x = x;
            s_last_y = y;
            if (!s_long_fired && now - s_down_ms >= DASH_TOUCH_LONG_MS
                && abs(x - s_down_x) < LONG_MAX_MOVE
                && abs(y - s_down_y) < LONG_MAX_MOVE) {
                s_long_fired = true;
                ev.type = DASH_TOUCH_LONG;
            }
        }
        return ev;
    }
    if (s_down && ++s_up_count >= RELEASE_SAMPLES) {
        s_down = false;
        s_up_count = 0;
        // A long press already acted; its release is not also a tap/swipe.
        if (!s_long_fired) ev = gesture_end(now);
    }
    if (!s_down && s_irq_ok) {
        // Idle: (re-)arm the pen-down interrupt.
        gpio_intr_enable(PIN_IRQ);
    }
    return ev;
}

void dash_touch_debug(char *buf, size_t n) {
    int irq = s_ok ? gpio_get_level(PIN_IRQ) : -1;
    snprintf(buf, n,
             "touch ok=%d irq=%d raw_x=%u raw_y=%u z=%u down=%d last=(%d,%d)",
             s_ok, irq, s_raw_x, s_raw_y, s_raw_z, s_down, s_last_x, s_last_y);
}
