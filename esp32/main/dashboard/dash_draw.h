/*
 * Dashboard drawing primitives: RGB565 strip rendering with anti-aliased
 * text (Inter, see dash_assets.h), rounded rectangles and lines. Screens
 * render in horizontal strips to stay within the no-PSRAM RAM budget; every
 * primitive clips to the strip it is given (screen rows [sy0, sy0 + sh)).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "dash_assets.h"

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
#define DASH_BG     DASH_RGB(10, 15, 26)     // near-black navy
#define DASH_CARD   DASH_RGB(21, 29, 45)
#define DASH_CARD2  DASH_RGB(30, 41, 62)     // raised / pressed
#define DASH_TEXT   DASH_RGB(234, 240, 250)
#define DASH_TEXT2  DASH_RGB(140, 154, 179)  // secondary
#define DASH_TEXT3  DASH_RGB(86, 98, 122)    // faint
#define DASH_ACCENT DASH_RGB(255, 176, 32)   // amber
#define DASH_BLUE   DASH_RGB(90, 160, 255)
#define DASH_UP     DASH_RGB(52, 211, 153)
#define DASH_DOWN   DASH_RGB(248, 113, 113)
#define DASH_BLACK  DASH_RGB(0, 0, 0)
#define DASH_WHITE  DASH_RGB(255, 255, 255)

static inline uint16_t dash_rgb(uint8_t r, uint8_t g, uint8_t b) {
    return DASH_RGB(r, g, b);
}

// Mix two palette colours: alpha 0 = a, 255 = b.
uint16_t dash_mix(uint16_t a, uint16_t b, int alpha);

// Fill [x0,x1) x [y0,y1) with c, clipped to the strip.
void dash_fill(uint16_t *buf, int sy0, int sh,
               int x0, int y0, int x1, int y1, uint16_t c);

// Filled rectangle with anti-aliased rounded corners of radius r.
void dash_round_rect(uint16_t *buf, int sy0, int sh,
                     int x0, int y0, int x1, int y1, int r, uint16_t c);

// Anti-aliased filled circle and line (width ~ w px).
void dash_circle(uint16_t *buf, int sy0, int sh, float cx, float cy, float r,
                 uint16_t c);
void dash_line(uint16_t *buf, int sy0, int sh, float x0, float y0, float x1,
               float y1, float w, uint16_t c);

// Text with its line top at y. Returns the pen advance (width).
int dash_text(uint16_t *buf, int sy0, int sh, const dash_font_t *f,
              int x, int y, const char *s, uint16_t c);
int dash_text_w(const dash_font_t *f, const char *s);
// Right-aligned ending at x1; centred in [x0, x1).
void dash_text_r(uint16_t *buf, int sy0, int sh, const dash_font_t *f,
                 int x1, int y, const char *s, uint16_t c);
void dash_text_c(uint16_t *buf, int sy0, int sh, const dash_font_t *f,
                 int x0, int x1, int y, const char *s, uint16_t c);

// Copy src into dst (size n), cut with "..." to fit max_w pixels.
void dash_text_fit(const dash_font_t *f, char *dst, int n, const char *src,
                   int max_w);

// Blit a 4-bit alpha bitmap (w x h, packed as in dash_assets) in colour c.
void dash_alpha(uint16_t *buf, int sy0, int sh, int x, int y, int w, int h,
                const uint8_t *bits, uint16_t c);

#ifdef __cplusplus
}
#endif
