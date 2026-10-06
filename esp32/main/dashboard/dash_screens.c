/*
 * Dashboard screens implementation.
 */
#include "dash_screens.h"

#include <stdio.h>
#include <string.h>

#include "dash_draw.h"
#include "dash_store.h"

#define TOP_H 30
#define NAV_H 28
#define CONTENT_Y TOP_H
#define CONTENT_H (DASH_H - TOP_H - NAV_H)

const char *dash_screen_name(dash_screen_t s) {
    switch (s) {
        case DASH_SCREEN_STOCKS: return "STOCKS";
        case DASH_SCREEN_WEATHER: return "WEATHER";
        case DASH_SCREEN_CALENDAR: return "CALENDAR";
        default: return "";
    }
}

// Bottom nav: "<" left, dots center, ">" right.
static void draw_nav(uint16_t *buf, int sy0, int sh, dash_screen_t cur) {
    dash_fill(buf, sy0, sh, 0, DASH_H - NAV_H, DASH_W, DASH_H, DASH_BLACK);
    // Separator line.
    dash_fill(buf, sy0, sh, 0, DASH_H - NAV_H, DASH_W, DASH_H - NAV_H + 1,
              dash_rgb(40, 55, 80));
    int cy = DASH_H - NAV_H + 6;
    dash_text(buf, sy0, sh, 14, cy, "<", 3, DASH_DIM, DASH_BLACK, true);
    dash_text(buf, sy0, sh, DASH_W - 14 - dash_text_w(">", 3), cy, ">", 3,
              DASH_DIM, DASH_BLACK, true);
    // Dots.
    int dw = DASH_SCREEN_COUNT * 14;
    int dx = (DASH_W - dw) / 2 + 3;
    for (int i = 0; i < DASH_SCREEN_COUNT; i++) {
        uint16_t c = (i == (int)cur) ? DASH_ACCENT : dash_rgb(60, 70, 95);
        dash_fill(buf, sy0, sh, dx + i * 14, cy + 4, dx + i * 14 + 8, cy + 10, c);
        dx += 0;
    }
}

static void draw_top(uint16_t *buf, int sy0, int sh,
                     dash_screen_t cur, const char *right) {
    dash_fill(buf, sy0, sh, 0, 0, DASH_W, TOP_H, dash_rgb(16, 28, 48));
    dash_text(buf, sy0, sh, 12, 8, dash_screen_name(cur), 2, DASH_ACCENT,
              DASH_BLACK, true);
    if (right && right[0]) {
        dash_text_r(buf, sy0, sh, DASH_W - 12, 10, right, 1, DASH_DIM,
                    DASH_BLACK, true);
    }
    dash_fill(buf, sy0, sh, 0, TOP_H - 1, DASH_W, TOP_H, dash_rgb(40, 55, 80));
}

static void draw_stocks(uint16_t *buf, int sy0, int sh) {
    int64_t age = dash_store_age_s();
    char right[24];
    if (age == INT64_MAX) {
        snprintf(right, sizeof(right), "waiting...");
    } else if (age < 90) {
        snprintf(right, sizeof(right), "live");
    } else if (age < 3600) {
        snprintf(right, sizeof(right), "%dm ago", (int)(age / 60));
    } else {
        snprintf(right, sizeof(right), "%dh ago", (int)(age / 3600));
    }

    dash_store_t *st = dash_store_lock();
    int n = st->n_stocks;
    if (n > DASH_MAX_STOCKS) n = DASH_MAX_STOCKS;
    dash_stock_t cp[DASH_MAX_STOCKS];
    memcpy(cp, st->stocks, (size_t)n * sizeof(dash_stock_t));
    dash_store_unlock();

    draw_top(buf, sy0, sh, DASH_SCREEN_STOCKS, right);

    int rows = n > 5 ? 5 : n;
    int row_h = CONTENT_H / 5;
    char price[24], chg[24];
    for (int i = 0; i < rows; i++) {
        int y = CONTENT_Y + i * row_h;
        const dash_stock_t *s = &cp[i];
        uint16_t chgc = s->change >= 0 ? DASH_GREEN : DASH_RED;
        // Card background.
        dash_fill(buf, sy0, sh, 8, y + 3, DASH_W - 8, y + row_h - 1, DASH_CARD);
        dash_text(buf, sy0, sh, 16, y + 8, s->symbol, 2, DASH_WHITE,
                  DASH_BLACK, true);
        snprintf(price, sizeof(price), "%.2f", s->price);
        dash_text_r(buf, sy0, sh, DASH_W - 16, y + 8, price, 2, DASH_WHITE,
                    DASH_BLACK, true);
        snprintf(chg, sizeof(chg), "%+.2f (%+.2f%%)", s->change, s->change_pct);
        dash_text(buf, sy0, sh, 16, y + 24, chg, 1, chgc, DASH_BLACK, true);
        if (s->market[0]) {
            dash_text_r(buf, sy0, sh, DASH_W - 16, y + 24, s->market, 1,
                        DASH_DIM, DASH_BLACK, true);
        }
    }
    if (n == 0) {
        dash_text_c(buf, sy0, sh, 0, DASH_W, CONTENT_Y + 60, "waiting for data",
                    2, DASH_DIM, DASH_BLACK, true);
    }
    draw_nav(buf, sy0, sh, DASH_SCREEN_STOCKS);
}

static void draw_weather(uint16_t *buf, int sy0, int sh) {
    dash_store_t *st = dash_store_lock();
    bool valid = st->weather_valid;
    dash_weather_t wx = st->weather;  // struct copy
    dash_store_unlock();

    draw_top(buf, sy0, sh, DASH_SCREEN_WEATHER,
             valid ? "Austin, TX" : NULL);

    if (!valid) {
        dash_text_c(buf, sy0, sh, 0, DASH_W, CONTENT_Y + 60, "waiting for data",
                    2, DASH_DIM, DASH_BLACK, true);
        draw_nav(buf, sy0, sh, DASH_SCREEN_WEATHER);
        return;
    }
    char tmp[16], hl[32], hum[32];
    snprintf(tmp, sizeof(tmp), "%dF", wx.temp);
    // Big temperature.
    dash_text(buf, sy0, sh, 18, CONTENT_Y + 12, tmp, 6, DASH_WHITE,
              DASH_BLACK, true);
    dash_text(buf, sy0, sh, 18 + dash_text_w(tmp, 6) + 12, CONTENT_Y + 18,
              wx.desc, 2, DASH_ACCENT, DASH_BLACK, true);
    snprintf(hl, sizeof(hl), "Feels %dF", wx.feels);
    dash_text(buf, sy0, sh, 20, CONTENT_Y + 78, hl, 2, DASH_DIM, DASH_BLACK,
              true);
    snprintf(hum, sizeof(hum), "H %d%%  W %dmph", wx.humidity, wx.wind);
    dash_text(buf, sy0, sh, 20, CONTENT_Y + 100, hum, 1, DASH_DIM, DASH_BLACK,
              true);
    // Forecast strip.
    int fw = DASH_W / 4;
    char fhl[16];
    for (int i = 0; i < wx.forecast_n && i < 4; i++) {
        int x0 = i * fw;
        dash_text_c(buf, sy0, sh, x0, x0 + fw, CONTENT_Y + 128,
                    wx.forecast[i].day, 1, DASH_DIM, DASH_BLACK, true);
        snprintf(fhl, sizeof(fhl), "%d/%d", wx.forecast[i].high,
                 wx.forecast[i].low);
        dash_text_c(buf, sy0, sh, x0, x0 + fw, CONTENT_Y + 142, fhl, 2,
                    DASH_WHITE, DASH_BLACK, true);
    }
    draw_nav(buf, sy0, sh, DASH_SCREEN_WEATHER);
}

static void draw_calendar(uint16_t *buf, int sy0, int sh) {
    dash_store_t *st = dash_store_lock();
    int n = st->n_events;
    char label[32];
    strncpy(label, st->events_label, sizeof(label) - 1);
    label[sizeof(label) - 1] = '\0';
    dash_event_t evs[DASH_MAX_EVENTS];
    if (n > DASH_MAX_EVENTS) n = DASH_MAX_EVENTS;
    memcpy(evs, st->events, (size_t)n * sizeof(dash_event_t));
    dash_store_unlock();

    draw_top(buf, sy0, sh, DASH_SCREEN_CALENDAR,
             label[0] ? label : NULL);

    if (n == 0) {
        dash_text_c(buf, sy0, sh, 0, DASH_W, CONTENT_Y + 60, "no events", 2,
                    DASH_DIM, DASH_BLACK, true);
        draw_nav(buf, sy0, sh, DASH_SCREEN_CALENDAR);
        return;
    }
    int rows = n > 4 ? 4 : n;
    int row_h = CONTENT_H / 4;
    for (int i = 0; i < rows; i++) {
        int y = CONTENT_Y + i * row_h;
        dash_fill(buf, sy0, sh, 8, y + 3, DASH_W - 8, y + row_h - 1, DASH_CARD);
        dash_text(buf, sy0, sh, 16, y + 8, evs[i].time, 1, DASH_ACCENT,
                  DASH_BLACK, true);
        // Truncate title to fit.
        char title[48];
        strncpy(title, evs[i].title, sizeof(title) - 1);
        title[sizeof(title) - 1] = '\0';
        while (dash_text_w(title, 2) > DASH_W - 40 && strlen(title) > 4) {
            title[strlen(title) - 1] = '\0';
        }
        dash_text(buf, sy0, sh, 16, y + 22, title, 2, DASH_WHITE, DASH_BLACK,
                  true);
    }
    draw_nav(buf, sy0, sh, DASH_SCREEN_CALENDAR);
}

void dash_screen_draw_strip(dash_screen_t s, uint16_t *buf, int sy0, int sh) {
    // Clear the strip first.
    for (int i = 0; i < DASH_W * sh; i++) buf[i] = DASH_BG;
    switch (s) {
        case DASH_SCREEN_STOCKS: draw_stocks(buf, sy0, sh); break;
        case DASH_SCREEN_WEATHER: draw_weather(buf, sy0, sh); break;
        case DASH_SCREEN_CALENDAR: draw_calendar(buf, sy0, sh); break;
        default: break;
    }
}
