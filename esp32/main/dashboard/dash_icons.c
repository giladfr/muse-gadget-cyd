/*
 * Weather icons: two anti-aliased alpha layers each (see dash_assets.c),
 * coloured here.
 */
#include "dash_icons.h"

#include "dash_assets.h"
#include "dash_draw.h"

static const struct {
    uint16_t primary, secondary;
} s_colors[DASH_ICON_COUNT] = {
    [DASH_ICON_SUN] = {DASH_ACCENT, DASH_ACCENT},
    [DASH_ICON_PARTLY] = {DASH_TEXT, DASH_ACCENT},
    [DASH_ICON_CLOUD] = {DASH_TEXT, DASH_TEXT},
    [DASH_ICON_FOG] = {DASH_TEXT2, DASH_TEXT2},
    [DASH_ICON_RAIN] = {DASH_TEXT, DASH_BLUE},
    [DASH_ICON_SNOW] = {DASH_TEXT, DASH_TEXT},
    [DASH_ICON_THUNDER] = {DASH_TEXT2, DASH_ACCENT},
};

dash_icon_t dash_icon_for_code(int code) {
    if (code == 0) return DASH_ICON_SUN;
    if (code == 1 || code == 2) return DASH_ICON_PARTLY;
    if (code == 3) return DASH_ICON_CLOUD;
    if (code == 45 || code == 48) return DASH_ICON_FOG;
    if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82)) {
        return DASH_ICON_RAIN;
    }
    if ((code >= 71 && code <= 77) || code == 85 || code == 86) {
        return DASH_ICON_SNOW;
    }
    if (code >= 95 && code <= 99) return DASH_ICON_THUNDER;
    return DASH_ICON_NONE;
}

void dash_icon(uint16_t *buf, int sy0, int sh, int x, int y, dash_icon_t icon,
               int size) {
    if (icon < 0 || icon >= DASH_ICON_COUNT) return;
    const dash_icon_img_t *img = size >= 56 ? &dash_icons56[icon]
                                            : &dash_icons24[icon];
    int n = img->size;
    if (y + n <= sy0 || y >= sy0 + sh) return;
    dash_alpha(buf, sy0, sh, x, y, n, n, img->primary, s_colors[icon].primary);
    dash_alpha(buf, sy0, sh, x, y, n, n, img->secondary,
               s_colors[icon].secondary);
}
