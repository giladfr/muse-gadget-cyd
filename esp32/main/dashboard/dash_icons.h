/*
 * Weather icons for the dashboard (anti-aliased, 56 px and 24 px).
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DASH_ICON_NONE = -1,
    DASH_ICON_SUN = 0,
    DASH_ICON_PARTLY,
    DASH_ICON_CLOUD,
    DASH_ICON_FOG,
    DASH_ICON_RAIN,
    DASH_ICON_SNOW,
    DASH_ICON_THUNDER,
    DASH_ICON_COUNT,
} dash_icon_t;

// Icon for an Open-Meteo weather code (DASH_ICON_NONE if unknown).
dash_icon_t dash_icon_for_code(int code);

// Draw `icon` (size 56 or 24) with its top-left at (x, y), clipped to the
// strip.
void dash_icon(uint16_t *buf, int sy0, int sh, int x, int y, dash_icon_t icon,
               int size);

#ifdef __cplusplus
}
#endif
