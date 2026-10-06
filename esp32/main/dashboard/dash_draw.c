/*
 * Dashboard drawing primitives implementation.
 */
#include "dash_draw.h"

#include <string.h>

#include "pixel_font.h"

void dash_fill(uint16_t *buf, int sy0, int sh,
               int x0, int y0, int x1, int y1, uint16_t c) {
    if (x0 < 0) x0 = 0;
    if (x1 > DASH_W) x1 = DASH_W;
    if (y0 < sy0) y0 = sy0;
    if (y1 > sy0 + sh) y1 = sy0 + sh;
    if (x0 >= x1 || y0 >= y1) return;
    for (int y = y0; y < y1; y++) {
        uint16_t *row = buf + (y - sy0) * DASH_W;
        for (int x = x0; x < x1; x++) row[x] = c;
    }
}

int dash_text_w(const char *s, int scale) {
    int n = 0;
    for (const char *p = s; *p; p++) n++;
    if (n == 0) return 0;
    return n * (PIXEL_FONT_WIDTH + 1) * scale - scale;
}

static void draw_glyph(uint16_t *buf, int sy0, int sh,
                       int x, int y, char ch, int scale,
                       uint16_t fg, uint16_t bg, bool transparent) {
    if ((unsigned char)ch < PIXEL_FONT_FIRST || (unsigned char)ch > PIXEL_FONT_LAST)
        ch = '?';
    const uint8_t *g = pixel_font[ch - PIXEL_FONT_FIRST];
    for (int gx = 0; gx < PIXEL_FONT_WIDTH; gx++) {
        uint8_t col = g[gx];
        for (int gy = 0; gy < PIXEL_FONT_HEIGHT; gy++) {
            bool on = (col >> gy) & 1;
            if (!on && transparent) continue;
            uint16_t c = on ? fg : bg;
            int px0 = x + gx * scale;
            int py0 = y + gy * scale;
            for (int dy = 0; dy < scale; dy++) {
                int py = py0 + dy;
                if (py < sy0 || py >= sy0 + sh || py < 0 || py >= DASH_H) continue;
                uint16_t *row = buf + (py - sy0) * DASH_W;
                for (int dx = 0; dx < scale; dx++) {
                    int px = px0 + dx;
                    if (px < 0 || px >= DASH_W) continue;
                    row[px] = c;
                }
            }
        }
    }
}

void dash_text(uint16_t *buf, int sy0, int sh,
               int x, int y, const char *s, int scale,
               uint16_t fg, uint16_t bg, bool transparent) {
    // Only glyphs intersecting the strip matter.
    int adv = (PIXEL_FONT_WIDTH + 1) * scale;
    for (const char *p = s; *p; p++, x += adv) {
        if (x + PIXEL_FONT_WIDTH * scale < 0 || x >= DASH_W) continue;
        if (y + PIXEL_FONT_HEIGHT * scale < sy0 || y >= sy0 + sh) continue;
        draw_glyph(buf, sy0, sh, x, y, *p, scale, fg, bg, transparent);
    }
}

void dash_text_r(uint16_t *buf, int sy0, int sh,
                 int x1, int y, const char *s, int scale,
                 uint16_t fg, uint16_t bg, bool transparent) {
    dash_text(buf, sy0, sh, x1 - dash_text_w(s, scale), y, s, scale,
              fg, bg, transparent);
}

void dash_text_c(uint16_t *buf, int sy0, int sh,
                 int xc0, int xc1, int y, const char *s, int scale,
                 uint16_t fg, uint16_t bg, bool transparent) {
    int w = dash_text_w(s, scale);
    dash_text(buf, sy0, sh, xc0 + (xc1 - xc0 - w) / 2, y, s, scale,
              fg, bg, transparent);
}
