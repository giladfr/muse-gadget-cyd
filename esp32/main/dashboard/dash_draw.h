/*
 * Dashboard drawing primitives: RGB565 strip rendering with the SDK's
 * 5x8 pixel font. Screens render in horizontal strips to stay within the
 * no-PSRAM RAM budget.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DASH_W 320
#define DASH_H 240
// 4-row strips, double-buffered: one strip is filled while the other goes out
// over SPI DMA. 2 x 2.5 KB keeps the same 5 KB footprint as a single 8-row
// strip, leaving heap for the image downloader (needs ~16 KB free).
#define DASH_STRIP_H 4

// The CYD panel takes RGB565 high byte first in memory (the same as the image
// wire format and lcd_px() in led_status.c), so colours are stored
// byte-swapped. DASH_RGB is a constant expression for use in the palette.
#define DASH_RGB565_(r, g, b) \
    ((((r) & 0xf8) << 8) | (((g) & 0xfc) << 3) | (((b) & 0xff) >> 3))
#define DASH_RGB(r, g, b) \
    ((uint16_t)(((DASH_RGB565_(r, g, b) >> 8) & 0xff) \
                | ((DASH_RGB565_(r, g, b) & 0xff) << 8)))

// Palette (panel byte order).
#define DASH_BG     DASH_RGB(10, 18, 32)     // dark navy
#define DASH_CARD   DASH_RGB(22, 34, 56)     // card
#define DASH_TOPBAR DASH_RGB(16, 28, 48)
#define DASH_RULE   DASH_RGB(40, 55, 80)     // separator lines
#define DASH_DOT    DASH_RGB(60, 70, 95)     // inactive nav dot
#define DASH_ACCENT DASH_RGB(255, 178, 0)    // amber
#define DASH_WHITE  DASH_RGB(235, 240, 248)  // off-white
#define DASH_DIM    DASH_RGB(140, 155, 180)  // dim blue-grey
#define DASH_GREEN  DASH_RGB(40, 210, 90)
#define DASH_RED    DASH_RGB(240, 60, 60)
#define DASH_BLACK  DASH_RGB(0, 0, 0)

static inline uint16_t dash_rgb(uint8_t r, uint8_t g, uint8_t b) {
    return DASH_RGB(r, g, b);
}

// Fill [x0,x1) of the strip (strip covers screen rows [sy0, sy0+sh)) with c,
// for screen rows [y0, y1) clipped to the strip.
void dash_fill(uint16_t *buf, int sy0, int sh,
               int x0, int y0, int x1, int y1, uint16_t c);

// Draw text at (x, y) clipped to the strip. bg is used unless transparent.
void dash_text(uint16_t *buf, int sy0, int sh,
               int x, int y, const char *s, int scale,
               uint16_t fg, uint16_t bg, bool transparent);

// Pixel width of s at scale (5px glyph + 1px spacing).
int dash_text_w(const char *s, int scale);

// Draw text right-aligned ending at x1.
void dash_text_r(uint16_t *buf, int sy0, int sh,
                 int x1, int y, const char *s, int scale,
                 uint16_t fg, uint16_t bg, bool transparent);

// Draw text centred in [xc0, xc1).
void dash_text_c(uint16_t *buf, int sy0, int sh,
                 int xc0, int xc1, int y, const char *s, int scale,
                 uint16_t fg, uint16_t bg, bool transparent);

#ifdef __cplusplus
}
#endif
