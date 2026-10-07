/*
 * Boot splash animation: Muse's mascot pops up and cheers (frames generated
 * by assets/gen_splash.py from the avatar renderer).
 *
 * The frames are deltas, decoded into one 64x64 4-bit buffer (2 KB) that
 * exists only between dash_splash_begin() and dash_splash_end().
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "dash_draw.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DASH_SPLASH_FRAMES 35
#define DASH_SPLASH_FPS 12
#define DASH_SPLASH_GRID 64
// Each grid pixel becomes SCALE x SCALE screen pixels.
#define DASH_SPLASH_SCALE 3
#define DASH_SPLASH_SIZE (DASH_SPLASH_GRID * DASH_SPLASH_SCALE)

extern const uint16_t dash_splash_palette[16];
extern const uint32_t dash_splash_offsets[DASH_SPLASH_FRAMES];
extern const uint8_t dash_splash_data[];

// Allocate the frame buffer and rewind. False without the RAM (show a static
// splash instead).
bool dash_splash_begin(void);
// Decode up to `frame` (clamped to the last one). Going back rewinds.
void dash_splash_seek(int frame);
// Draw the current frame with its top-left at (x, y), clipped to the strip;
// transparent pixels keep what is under them.
void dash_splash_draw(uint16_t *buf, int sy0, int sh, int x, int y);
// Free the frame buffer.
void dash_splash_end(void);

#ifdef __cplusplus
}
#endif
