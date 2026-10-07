/*
 * Boot splash animation decoder (format: assets/gen_splash.py).
 */
#include "dash_splash.h"

#include <stdlib.h>
#include <string.h>

#define ROW_BYTES (DASH_SPLASH_GRID / 2)

static uint8_t *s_fb;  // 64 rows of 32 bytes, high nibble = left pixel
static int s_frame = -1;

bool dash_splash_begin(void) {
    if (!s_fb) s_fb = malloc(DASH_SPLASH_GRID * ROW_BYTES);
    if (!s_fb) return false;
    memset(s_fb, 0, DASH_SPLASH_GRID * ROW_BYTES);
    s_frame = -1;
    return true;
}

void dash_splash_end(void) {
    free(s_fb);
    s_fb = NULL;
    s_frame = -1;
}

static void set_px(uint8_t *row, int x, int v) {
    uint8_t *b = &row[x >> 1];
    *b = x & 1 ? (uint8_t)((*b & 0xf0) | v) : (uint8_t)((*b & 0x0f) | v << 4);
}

static void apply(int frame) {
    const uint8_t *p = dash_splash_data + dash_splash_offsets[frame];
    for (int y = 0; y < DASH_SPLASH_GRID; y++) {
        uint8_t *row = s_fb + y * ROW_BYTES;
        int n = *p++, x = 0;
        while (n--) {
            int skip = *p >> 4, count = *p & 15;
            p++;
            x += skip;
            for (int i = 0; i < count && x < DASH_SPLASH_GRID; i++, x++) {
                uint8_t b = p[i >> 1];
                set_px(row, x, i & 1 ? b & 15 : b >> 4);
            }
            p += (count + 1) / 2;
        }
    }
}

void dash_splash_seek(int frame) {
    if (!s_fb) return;
    if (frame >= DASH_SPLASH_FRAMES) frame = DASH_SPLASH_FRAMES - 1;
    if (frame < s_frame) {
        memset(s_fb, 0, DASH_SPLASH_GRID * ROW_BYTES);
        s_frame = -1;
    }
    while (s_frame < frame) apply(++s_frame);
}

void dash_splash_draw(uint16_t *buf, int sy0, int sh, int x, int y) {
    if (!s_fb || s_frame < 0) return;
    int ya = sy0 > y ? sy0 : y;
    int yb = sy0 + sh < y + DASH_SPLASH_SIZE ? sy0 + sh : y + DASH_SPLASH_SIZE;
    for (int sy = ya; sy < yb; sy++) {
        const uint8_t *row = s_fb + (sy - y) / DASH_SPLASH_SCALE * ROW_BYTES;
        uint16_t *out = buf + (sy - sy0) * DASH_W;
        for (int gx = 0; gx < DASH_SPLASH_GRID; gx++) {
            int v = gx & 1 ? row[gx >> 1] & 15 : row[gx >> 1] >> 4;
            if (!v) continue;
            int sx = x + gx * DASH_SPLASH_SCALE;
            for (int k = 0; k < DASH_SPLASH_SCALE; k++) {
                if (sx + k >= 0 && sx + k < DASH_W) out[sx + k] = dash_splash_palette[v];
            }
        }
    }
}
