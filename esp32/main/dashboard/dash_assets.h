/*
 * Generated dashboard assets (see assets/gen_assets.py): anti-aliased Inter
 * fonts and weather icons, stored as 4-bit alpha in flash.
 */
#pragma once

#include <stdint.h>

#include "dash_icons.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t offset;  // into the font's bits
    uint8_t w, h;     // bitmap size (0 for blank glyphs)
    int8_t x_off;     // bitmap left, from the pen position
    int8_t y_off;     // bitmap top, from the line top
    uint8_t adv;      // pen advance
} dash_glyph_t;

// ASCII 32..127; 127 holds the degree sign (write "\x7f").
typedef struct {
    const uint8_t *bits;
    const dash_glyph_t *glyphs;
    uint8_t line_h;
    uint8_t ascent;
} dash_font_t;

#define DASH_DEG "\x7f"

extern const dash_font_t dash_font_small;  // Inter Medium 12
extern const dash_font_t dash_font_body;   // Inter SemiBold 16
extern const dash_font_t dash_font_large;  // Inter SemiBold 22
extern const dash_font_t dash_font_huge;   // Inter Display SemiBold 58, digits

typedef struct {
    uint8_t size;              // square
    const uint8_t *primary;    // 4-bit alpha layers
    const uint8_t *secondary;
} dash_icon_img_t;

extern const dash_icon_img_t dash_icons56[DASH_ICON_COUNT];
extern const dash_icon_img_t dash_icons24[DASH_ICON_COUNT];

#ifdef __cplusplus
}
#endif
