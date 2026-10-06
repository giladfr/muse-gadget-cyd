// sd_jpeg.c — decode a baseline JPEG from /sdcard and show it full-screen.
//
// Streams the file through the ROM TJpgDec; no full-file RAM copy needed.
// On success the dashboard enters takeover mode (same X-to-dismiss as
// display.draw_url).

#include "sd_jpeg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rom/tjpgd.h"
#include "esp_log.h"

#include "led_status.h"
#include "dashboard/dashboard.h"
#include "sd_card.h"

static const char *TAG = "sd.jpeg";

// Work pool for the ROM decoder, per its documentation.
#define JPEG_POOL_BYTES 3100
// Largest block the decoder emits: a 16x16 MCU.
#define JPEG_MCU_PIXELS (16 * 16)

typedef struct {
    FILE *f;
    uint16_t mcu[JPEG_MCU_PIXELS];
    int x0, y0;
    int width, height;
} sd_jpeg_t;

static UINT sd_jpeg_in(JDEC *jd, BYTE *buf, UINT len) {
    sd_jpeg_t *j = jd->device;
    if (buf) {
        size_t n = fread(buf, 1, len, j->f);
        return (UINT)n;
    }
    // Skip: read-and-discard in MCU-sized chunks.
    UINT skipped = 0;
    uint8_t tmp[256];
    while (skipped < len) {
        UINT chunk = len - skipped;
        if (chunk > sizeof(tmp)) chunk = sizeof(tmp);
        size_t n = fread(tmp, 1, chunk, j->f);
        if (n == 0) break;
        skipped += (UINT)n;
    }
    return skipped;
}

static UINT sd_jpeg_out(JDEC *jd, void *bitmap, JRECT *rect) {
    sd_jpeg_t *j = jd->device;
    int w = rect->right - rect->left + 1;
    int h = rect->bottom - rect->top + 1;
    const uint8_t *rgb = bitmap;
    for (int i = 0; i < w * h; i++, rgb += 3) {
        uint16_t px = ((rgb[0] & 0xF8) << 8) | ((rgb[1] & 0xFC) << 3)
                      | (rgb[2] >> 3);
        j->mcu[i] = (uint16_t)((px >> 8) | (px << 8));
    }
    // led_status_draw_rect() returns true on success; 0 aborts the decode.
    return led_status_draw_rect(j->x0 + rect->left, j->y0 + rect->top,
                                w, h, j->mcu) ? 1 : 0;
}

const char *sd_jpeg_show(const char *path) {
    if (!sd_card_mounted() && !sd_card_init()) {
        return "no SD card";
    }
    char full[128];
    snprintf(full, sizeof(full), "/sdcard/%s", path);
    // Reject path traversal.
    if (strstr(path, "..")) return "invalid path";

    FILE *f = fopen(full, "rb");
    if (!f) return "file not found";

    sd_jpeg_t j = {.f = f, .width = 320, .height = 240};
    void *pool = malloc(JPEG_POOL_BYTES);
    if (!pool) {
        fclose(f);
        return "out of memory";
    }

    // Keep the dashboard off the screen while the image is drawn.
    dashboard_takeover_prepare();

    const char *err = NULL;
    JDEC jd;
    JRESULT rc = jd_prepare(&jd, sd_jpeg_in, pool, JPEG_POOL_BYTES, &j);
    if (rc == JDR_OK) {
        uint8_t scale = 0;
        while (scale < 3 && ((int)(jd.width >> scale) > j.width
                             || (int)(jd.height >> scale) > j.height)) {
            scale++;
        }
        int w = (int)(jd.width >> scale), h = (int)(jd.height >> scale);
        if (w > j.width || h > j.height) {
            err = "JPEG too large even at 1/8";
        } else {
            j.x0 = (j.width - w) / 2;
            j.y0 = (j.height - h) / 2;
            // Clear to black around a smaller image, a few rows at a time
            // (led_status_draw_rect copies, so any buffer will do).
            if (w < j.width || h < j.height) {
                const int rows = 4;
                uint16_t *black = calloc((size_t)j.width * rows, sizeof(uint16_t));
                if (!black) err = "out of memory";
                for (int y = 0; black && y < j.height && !err; y += rows) {
                    int n = j.height - y < rows ? j.height - y : rows;
                    if (!led_status_draw_rect(0, y, j.width, n, black)) {
                        err = "display write failed";
                    }
                }
                free(black);
            }
            if (!err) rc = jd_decomp(&jd, sd_jpeg_out, scale);
        }
    }
    if (!err && rc != JDR_OK) {
        err = rc == JDR_FMT3 ? "use baseline JPEG, not progressive"
            : "not a valid JPEG";
    }
    free(pool);
    fclose(f);
    if (err) {
        dashboard_takeover_end();
        return err;
    }

    ESP_LOGI(TAG, "showing %s, entering takeover", full);
    dashboard_takeover_begin();
    return NULL;
}
