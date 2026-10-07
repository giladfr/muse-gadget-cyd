// Shared state of the CYD simulator (see sim_platform.c, sim_main.c).
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint16_t fb[240][320];   // panel memory: RGB565, high byte first
    // The last complete frame (fb as of the end of the last full pass), for
    // recordings without half-drawn frames.
    uint16_t shown[240][320];
    volatile unsigned frames;
    // Touch (screen pixels).
    volatile bool pen;
    volatile int pen_x, pen_y;
    // Panel orientation quirks to emulate (test calibration with them).
    bool touch_swap, touch_invert_x, touch_invert_y;
    // Backlight duty (0..1023) and the light sensor's raw reading.
    volatile int backlight;
    volatile int ldr;
    // Link session up, Wi-Fi RSSI (0 with link down = no Wi-Fi).
    volatile bool link;
    volatile int rssi;
    // Muse acknowledges chat sends (false: it refuses them).
    volatile bool chat_ok;
    // Throttle panel transfers like a 40 MHz SPI bus.
    bool slow_spi;
    char nvs_path[256];
    // musegadget's local socket: chat sends go to the real Muse through it.
    char muse_socket[104];  // sun_path holds 104 bytes on macOS
} sim_state_t;

extern sim_state_t g_sim;

void sim_pen(bool down, int x, int y);
void sim_nvs_load(void);
void sim_fb_lock(void);
void sim_fb_unlock(void);
// Update `shown` if the panel has been idle a while; call with the fb lock.
void sim_fb_settle(void);
