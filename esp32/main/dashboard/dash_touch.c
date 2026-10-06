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

// Raw 12-bit range of the 2432S028R panel (community calibration for the
// landscape orientation the status screen uses).
#define RAW_X_MIN 200
#define RAW_X_MAX 3700
#define RAW_Y_MIN 240
#define RAW_Y_MAX 3800
// Below this pressure the reading is noise (finger lifting or not there).
#define Z_THRESHOLD 400

#define SWIPE_MIN_DX 40
#define TAP_MAX_MS 400
#define TAP_MAX_MOVE 12
// Consecutive pen-up samples before a touch counts as released, so one bad
// sample mid-swipe doesn't split the gesture.
#define RELEASE_SAMPLES 2

static bool s_ok = false;
static bool s_irq_ok = false;
static TaskHandle_t s_notify;

// Gesture state (dashboard task only).
static bool s_down = false;
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

    int px = map_axis(rx, RAW_X_MIN, RAW_X_MAX, 320);
    int py = map_axis(ry, RAW_Y_MIN, RAW_Y_MAX, 240);
#if CONFIG_HOMEHUB_DASHBOARD_TOUCH_SWAP_XY
    // Swapped panels: scale each raw axis onto the other screen axis.
    px = map_axis(ry, RAW_Y_MIN, RAW_Y_MAX, 320);
    py = map_axis(rx, RAW_X_MIN, RAW_X_MAX, 240);
#endif
#if CONFIG_HOMEHUB_DASHBOARD_TOUCH_INVERT_X
    px = 319 - px;
#endif
#if CONFIG_HOMEHUB_DASHBOARD_TOUCH_INVERT_Y
    py = 239 - py;
#endif
    *sx = px;
    *sy = py;
    return true;
}

bool dash_touch_init(TaskHandle_t notify) {
    s_notify = notify;
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
            s_down_x = s_last_x = x;
            s_down_y = s_last_y = y;
            s_down_ms = now;
        } else {
            s_last_x = x;
            s_last_y = y;
        }
        return ev;
    }
    if (s_down && ++s_up_count >= RELEASE_SAMPLES) {
        s_down = false;
        s_up_count = 0;
        ev = gesture_end(now);
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
