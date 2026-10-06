/*
 * Dashboard drawing primitives implementation.
 */
#include "dash_draw.h"

#include <math.h>
#include <string.h>

static inline uint16_t swap16(uint16_t v) {
    return (uint16_t)((v >> 8) | (v << 8));
}

uint16_t dash_mix(uint16_t a, uint16_t b, int alpha) {
    if (alpha <= 0) return a;
    if (alpha >= 255) return b;
    uint16_t x = swap16(a), y = swap16(b);
    int r = ((x >> 11) * (255 - alpha) + (y >> 11) * alpha) / 255;
    int g = (((x >> 5) & 63) * (255 - alpha) + ((y >> 5) & 63) * alpha) / 255;
    int bl = ((x & 31) * (255 - alpha) + (y & 31) * alpha) / 255;
    return swap16((uint16_t)((r << 11) | (g << 5) | bl));
}

static inline void blend(uint16_t *buf, int sy0, int sh, int x, int y,
                         uint16_t c, int alpha) {
    if (alpha <= 0 || x < 0 || x >= DASH_W || y < sy0 || y >= sy0 + sh) return;
    uint16_t *p = buf + (y - sy0) * DASH_W + x;
    *p = alpha >= 255 ? c : dash_mix(*p, c, alpha);
}

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

// Coverage (0..255) of pixel (px, py) by a circle; 2x2 supersampled edge.
static int circle_cov(float px, float py, float cx, float cy, float r) {
    float dx = px + 0.5f - cx, dy = py + 0.5f - cy;
    float d = sqrtf(dx * dx + dy * dy);
    if (d <= r - 0.7f) return 255;
    if (d >= r + 0.7f) return 0;
    int n = 0;
    for (int i = 0; i < 4; i++) {
        float sx = px + 0.25f + 0.5f * (i & 1) - cx;
        float sy = py + 0.25f + 0.5f * (i >> 1) - cy;
        if (sx * sx + sy * sy <= r * r) n++;
    }
    return n * 255 / 4;
}

void dash_round_rect(uint16_t *buf, int sy0, int sh,
                     int x0, int y0, int x1, int y1, int r, uint16_t c) {
    int ya = y0 > sy0 ? y0 : sy0, yb = y1 < sy0 + sh ? y1 : sy0 + sh;
    if (ya >= yb || x0 >= x1) return;
    if (r * 2 > y1 - y0) r = (y1 - y0) / 2;
    if (r * 2 > x1 - x0) r = (x1 - x0) / 2;
    for (int y = ya; y < yb; y++) {
        int inset = 0;
        float cy = -1;
        if (y < y0 + r) cy = (float)(y0 + r);
        else if (y >= y1 - r) cy = (float)(y1 - r);
        if (cy >= 0) {
            // Corner rows: blend the curve, fill the middle.
            for (int x = x0; x < x0 + r; x++) {
                int a = circle_cov((float)x, (float)y, (float)(x0 + r), cy, (float)r);
                blend(buf, sy0, sh, x, y, c, a);
                blend(buf, sy0, sh, x1 - 1 - (x - x0), y, c, a);
            }
            inset = r;
        }
        dash_fill(buf, sy0, sh, x0 + inset, y, x1 - inset, y + 1, c);
    }
}

void dash_circle(uint16_t *buf, int sy0, int sh, float cx, float cy, float r,
                 uint16_t c) {
    int ya = (int)floorf(cy - r - 1), yb = (int)ceilf(cy + r + 1);
    if (ya < sy0) ya = sy0;
    if (yb > sy0 + sh) yb = sy0 + sh;
    for (int y = ya; y < yb; y++) {
        for (int x = (int)floorf(cx - r - 1); x <= (int)ceilf(cx + r + 1); x++) {
            blend(buf, sy0, sh, x, y, c, circle_cov((float)x, (float)y, cx, cy, r));
        }
    }
}

void dash_line(uint16_t *buf, int sy0, int sh, float x0, float y0, float x1,
               float y1, float w, uint16_t c) {
    // Distance-to-segment coverage over the line's bounding box (clipped to
    // the strip, which keeps this cheap).
    float hw = w / 2;
    int ya = (int)floorf(fminf(y0, y1) - hw - 1);
    int yb = (int)ceilf(fmaxf(y0, y1) + hw + 1);
    if (ya < sy0) ya = sy0;
    if (yb > sy0 + sh) yb = sy0 + sh;
    if (ya >= yb) return;
    int xa = (int)floorf(fminf(x0, x1) - hw - 1);
    int xb = (int)ceilf(fmaxf(x0, x1) + hw + 1);
    float dx = x1 - x0, dy = y1 - y0, len2 = dx * dx + dy * dy;
    for (int y = ya; y < yb; y++) {
        for (int x = xa; x <= xb; x++) {
            float px = x + 0.5f, py = y + 0.5f;
            float t = len2 > 0 ? ((px - x0) * dx + (py - y0) * dy) / len2 : 0;
            if (t < 0) t = 0;
            if (t > 1) t = 1;
            float ex = px - (x0 + t * dx), ey = py - (y0 + t * dy);
            float d = sqrtf(ex * ex + ey * ey) - hw;  // <0 inside
            int a = d <= -0.5f ? 255 : d >= 0.5f ? 0 : (int)((0.5f - d) * 255);
            blend(buf, sy0, sh, x, y, c, a);
        }
    }
}

void dash_alpha(uint16_t *buf, int sy0, int sh, int x, int y, int w, int h,
                const uint8_t *bits, uint16_t c) {
    if (y + h <= sy0 || y >= sy0 + sh || w <= 0) return;
    int r0 = sy0 - y > 0 ? sy0 - y : 0;
    int r1 = sy0 + sh - y < h ? sy0 + sh - y : h;
    for (int r = r0; r < r1; r++) {
        int i = r * w;
        for (int col = 0; col < w; col++, i++) {
            int a4 = (i & 1) ? bits[i >> 1] & 15 : bits[i >> 1] >> 4;
            if (a4) blend(buf, sy0, sh, x + col, y + r, c, a4 * 17);
        }
    }
}

static const dash_glyph_t *glyph(const dash_font_t *f, char ch) {
    unsigned char u = (unsigned char)ch;
    if (u < 32 || u > 127) u = '?';
    return &f->glyphs[u - 32];
}

int dash_text_w(const dash_font_t *f, const char *s) {
    int w = 0;
    for (; *s; s++) w += glyph(f, *s)->adv;
    return w;
}

int dash_text(uint16_t *buf, int sy0, int sh, const dash_font_t *f,
              int x, int y, const char *s, uint16_t c) {
    int x0 = x;
    bool rows = y + f->line_h > sy0 && y < sy0 + sh;
    for (; *s; s++) {
        const dash_glyph_t *g = glyph(f, *s);
        if (rows && g->w) {
            dash_alpha(buf, sy0, sh, x + g->x_off, y + g->y_off, g->w, g->h,
                       f->bits + g->offset, c);
        }
        x += g->adv;
    }
    return x - x0;
}

void dash_text_r(uint16_t *buf, int sy0, int sh, const dash_font_t *f,
                 int x1, int y, const char *s, uint16_t c) {
    dash_text(buf, sy0, sh, f, x1 - dash_text_w(f, s), y, s, c);
}

void dash_text_c(uint16_t *buf, int sy0, int sh, const dash_font_t *f,
                 int x0, int x1, int y, const char *s, uint16_t c) {
    dash_text(buf, sy0, sh, f, x0 + (x1 - x0 - dash_text_w(f, s)) / 2, y, s, c);
}

void dash_text_fit(const dash_font_t *f, char *dst, int n, const char *src,
                   int max_w) {
    int len = (int)strlen(src);
    if (len > n - 1) len = n - 1;
    memcpy(dst, src, (size_t)len);
    dst[len] = '\0';
    if (dash_text_w(f, dst) <= max_w) return;
    int dots = dash_text_w(f, "...");
    while (len > 0 && (dash_text_w(f, dst) + dots > max_w || dst[len - 1] == ' ')) {
        dst[--len] = '\0';
    }
    if (len + 4 <= n) strcpy(dst + len, "...");
}
