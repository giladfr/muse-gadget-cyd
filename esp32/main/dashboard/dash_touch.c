/*
 * XPT2046 driver implementation. Shares SPI2_HOST with the ILI9341 display.
 */
#include "dash_touch.h"

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "dash.touch";

// 2432S028R touch wiring (shared SPI bus: SCK 14, MOSI 13, MISO 12).
#define TOUCH_HOST SPI2_HOST
#define TOUCH_SPI_HZ (2 * 1000 * 1000)

// Raw 12-bit ADC range of the panel (tune if edges misbehave).
#define RAW_MIN 200
#define RAW_MAX 3800
// Screen mapping: raw X -> screen X, raw Y -> screen Y. Flip here if needed.
#define SWAP_XY false
#define INVERT_X false
#define INVERT_Y true

#define SWIPE_MIN_DX 40
#define TAP_MAX_MS 400
#define TAP_MAX_MOVE 12

static spi_device_handle_t s_touch = NULL;
static bool s_ok = false;

// --- gesture state ---
static bool s_down = false;
static int s_down_x, s_down_y;
static uint32_t s_down_ms;
static int s_last_x, s_last_y;

static uint16_t tp_read(uint8_t cmd) {
    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    // 8-bit command, then 12-bit result. Use 24-bit transfer.
    uint8_t tx[3] = {cmd, 0x00, 0x00};
    uint8_t rx[3] = {0};
    t.length = 24;
    t.tx_buffer = tx;
    t.rx_buffer = rx;
    if (spi_device_polling_transmit(s_touch, &t) != ESP_OK) return 0;
    uint32_t v = ((uint32_t)rx[1] << 8 | rx[2]) >> 3;
    return (uint16_t)(v & 0xfff);
}

static bool tp_pressed(void) {
    return gpio_get_level(CONFIG_HOMEHUB_DASHBOARD_TOUCH_IRQ) == 0;
}

static bool tp_pos(int *sx, int *sy) {
    // Average a few samples for stability.
    uint32_t rx = 0, ry = 0;
    for (int i = 0; i < 4; i++) {
        rx += tp_read(0x90);  // X
        ry += tp_read(0xd0);  // Y
    }
    rx /= 4;
    ry /= 4;
    if (rx < RAW_MIN || rx > RAW_MAX || ry < RAW_MIN || ry > RAW_MAX) {
        return false;
    }
    int x = (int)((rx - RAW_MIN) * 320 / (RAW_MAX - RAW_MIN));
    int y = (int)((ry - RAW_MIN) * 240 / (RAW_MAX - RAW_MIN));
    if (x < 0) x = 0;
    if (x > 319) x = 319;
    if (y < 0) y = 0;
    if (y > 239) y = 239;
#if SWAP_XY
    int t = x; x = y; y = t;
#endif
#if INVERT_X
    x = 319 - x;
#endif
#if INVERT_Y
    y = 239 - y;
#endif
    *sx = x;
    *sy = y;
    return true;
}

bool dash_touch_init(void) {
    gpio_config_t irq = {
        .pin_bit_mask = 1ULL << CONFIG_HOMEHUB_DASHBOARD_TOUCH_IRQ,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    if (gpio_config(&irq) != ESP_OK) {
        ESP_LOGE(TAG, "IRQ gpio config failed");
        return false;
    }
    spi_device_interface_config_t dev = {
        .clock_speed_hz = TOUCH_SPI_HZ,
        .mode = 0,
        .spics_io_num = CONFIG_HOMEHUB_DASHBOARD_TOUCH_CS,
        .queue_size = 1,
    };
    // The display already initialized the bus; just add our device.
    if (spi_bus_add_device(TOUCH_HOST, &dev, &s_touch) != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_add_device failed");
        return false;
    }
    // Sanity: read once; all-zero/all-ones means no controller.
    uint16_t v = tp_read(0x90);
    if (v == 0 || v == 0xfff) {
        ESP_LOGW(TAG, "touch controller not answering (0x%03x)", v);
        // Keep going; a missing touch shouldn't kill the dashboard.
    } else {
        ESP_LOGI(TAG, "XPT2046 ready (probe 0x%03x)", v);
    }
    s_ok = true;
    return true;
}

dash_touch_t dash_touch_poll(void) {
    dash_touch_t ev = {.type = DASH_TOUCH_NONE, .x = 0, .y = 0};
    if (!s_ok) return ev;
    uint32_t now = xTaskGetTickCount() * 1000 / configTICK_RATE_HZ;
    if (tp_pressed()) {
        int x, y;
        if (!tp_pos(&x, &y)) return ev;
        if (!s_down) {
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
    if (s_down) {
        s_down = false;
        int dx = s_last_x - s_down_x;
        int dy = s_last_y - s_down_y;
        uint32_t dt = now - s_down_ms;
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
    }
    return ev;
}

void dash_touch_debug(char *buf, size_t n) {
    int irq = -1;
    uint16_t raw_x = 0, raw_y = 0;
    if (s_ok) {
        irq = gpio_get_level(CONFIG_HOMEHUB_DASHBOARD_TOUCH_IRQ);
        raw_x = tp_read(0x90);
        raw_y = tp_read(0xd0);
    }
    snprintf(buf, n,
             "touch ok=%d irq=%d raw_x=0x%03x raw_y=0x%03x down=%d last=(%d,%d)",
             s_ok, irq, raw_x, raw_y, s_down, s_last_x, s_last_y);
}
