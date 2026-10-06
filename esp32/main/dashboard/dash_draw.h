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
// 8-row strips: 5KB DMA buffer. Smaller strips leave more heap for the
// image downloader (needs ~16KB free).
#define DASH_STRIP_H 8

// Palette (RGB565).
#define DASH_BG     0x0a12  // dark navy (10,18,32)
#define DASH_CARD   0x1622  // card (22,34,56)
#define DASH_ACCENT 0xfd20  // amber (255,178,0)
#define DASH_WHITE  0xe73c  // off-white (235,240,248)
#define DASH_DIM    0x8c71  // dim blue-grey (140,155,180)
#define DASH_GREEN  0x07e0
#define DASH_RED    0xf800
#define DASH_BLACK  0x0000

static inline uint16_t dash_rgb(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3));
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
