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

#define STOCK_ROWS 5
#define STOCK_ROW_H (CONTENT_H / STOCK_ROWS)
#define EVENT_ROWS 4
#define EVENT_ROW_H (CONTENT_H / EVENT_ROWS)
// Calendar titles are drawn at scale 2 within DASH_W - 40 pixels.
#define TITLE_MAX_CHARS ((DASH_W - 40 + 2) / ((5 + 1) * 2))

typedef struct {
    char symbol[16];
    char price[16];
    char chg[32];
    char market[16];
    bool up;
} stock_row_t;

// Everything a screen draws, already formatted. Zeroed before it is built so
// two frames compare with memcmp.
typedef struct {
    bool valid;
    dash_screen_t screen;
    char right[32];  // header, right-aligned
    bool empty;      // show the "waiting"/"no events" message
    union {
        struct {
            stock_row_t rows[STOCK_ROWS];
            int n;
        } stocks;
        struct {
            char temp[8];
            char desc[24];
            char feels[24];
            char hum[32];
            char day[DASH_MAX_FORECAST][8];
            char hl[DASH_MAX_FORECAST][16];
            int n;
        } weather;
        struct {
            char time[EVENT_ROWS][16];
            char title[EVENT_ROWS][TITLE_MAX_CHARS + 1];
            int n;
        } cal;
    } u;
} frame_t;

static frame_t s_frame;
static frame_t s_prev;
static dash_store_t s_snap;  // static: keeps ~1.3 KB off the task stack

const char *dash_screen_name(dash_screen_t s) {
    switch (s) {
        case DASH_SCREEN_STOCKS: return "STOCKS";
        case DASH_SCREEN_WEATHER: return "WEATHER";
        case DASH_SCREEN_CALENDAR: return "CALENDAR";
        default: return "";
    }
}

// ---- frame building -------------------------------------------------------

static void build_stocks(frame_t *f, const dash_store_t *st) {
    int64_t age = dash_store_stocks_age_s(st);
    if (age == INT64_MAX) {
        snprintf(f->right, sizeof(f->right), "waiting...");
    } else if (age < 90) {
        snprintf(f->right, sizeof(f->right), "live");
    } else if (age < 3600) {
        snprintf(f->right, sizeof(f->right), "%dm ago", (int)(age / 60));
    } else if (age < 48 * 3600) {
        snprintf(f->right, sizeof(f->right), "%dh ago", (int)(age / 3600));
    } else {
        snprintf(f->right, sizeof(f->right), "%dd ago", (int)(age / 86400));
    }
    int n = st->n_stocks < STOCK_ROWS ? st->n_stocks : STOCK_ROWS;
    f->u.stocks.n = n;
    f->empty = n == 0;
    for (int i = 0; i < n; i++) {
        const dash_stock_t *s = &st->stocks[i];
        stock_row_t *r = &f->u.stocks.rows[i];
        snprintf(r->symbol, sizeof(r->symbol), "%s", s->symbol);
        snprintf(r->price, sizeof(r->price), "%.2f", s->price);
        snprintf(r->chg, sizeof(r->chg), "%+.2f (%+.2f%%)", s->change,
                 s->change_pct);
        snprintf(r->market, sizeof(r->market), "%s", s->market);
        r->up = s->change >= 0;
    }
}

static void build_weather(frame_t *f, const dash_store_t *st) {
    f->empty = !st->weather_valid;
    if (f->empty) return;
    const dash_weather_t *wx = &st->weather;
    snprintf(f->right, sizeof(f->right), "Austin, TX");
    snprintf(f->u.weather.temp, sizeof(f->u.weather.temp), "%dF", wx->temp);
    snprintf(f->u.weather.desc, sizeof(f->u.weather.desc), "%s", wx->desc);
    snprintf(f->u.weather.feels, sizeof(f->u.weather.feels), "Feels %dF",
             wx->feels);
    snprintf(f->u.weather.hum, sizeof(f->u.weather.hum), "H %d%%  W %dmph",
             wx->humidity, wx->wind);
    int n = wx->forecast_n < DASH_MAX_FORECAST ? wx->forecast_n : DASH_MAX_FORECAST;
    f->u.weather.n = n;
    for (int i = 0; i < n; i++) {
        snprintf(f->u.weather.day[i], sizeof(f->u.weather.day[i]), "%s",
                 wx->forecast[i].day);
        snprintf(f->u.weather.hl[i], sizeof(f->u.weather.hl[i]), "%d/%d",
                 wx->forecast[i].high, wx->forecast[i].low);
    }
}

static void build_calendar(frame_t *f, const dash_store_t *st) {
    snprintf(f->right, sizeof(f->right), "%s", st->events_label);
    int n = st->n_events < EVENT_ROWS ? st->n_events : EVENT_ROWS;
    f->u.cal.n = n;
    f->empty = n == 0;
    for (int i = 0; i < n; i++) {
        snprintf(f->u.cal.time[i], sizeof(f->u.cal.time[i]), "%s",
                 st->events[i].time);
        // Truncate to what fits (the buffer is exactly that size).
        snprintf(f->u.cal.title[i], sizeof(f->u.cal.title[i]), "%s",
                 st->events[i].title);
    }
}

static void mark(int *y0, int *y1, int a, int b) {
    if (*y0 == *y1) {
        *y0 = a;
        *y1 = b;
        return;
    }
    if (a < *y0) *y0 = a;
    if (b > *y1) *y1 = b;
}

void dash_screen_prepare(dash_screen_t s, bool full, int *y0, int *y1) {
    dash_store_snapshot(&s_snap);
    s_prev = s_frame;
    memset(&s_frame, 0, sizeof(s_frame));
    s_frame.valid = true;
    s_frame.screen = s;
    switch (s) {
        case DASH_SCREEN_STOCKS: build_stocks(&s_frame, &s_snap); break;
        case DASH_SCREEN_WEATHER: build_weather(&s_frame, &s_snap); break;
        case DASH_SCREEN_CALENDAR: build_calendar(&s_frame, &s_snap); break;
        default: break;
    }

    *y0 = *y1 = 0;
    if (full || !s_prev.valid || s_prev.screen != s) {
        *y1 = DASH_H;
        return;
    }
    if (strcmp(s_prev.right, s_frame.right) != 0) mark(y0, y1, 0, TOP_H);
    if (s_prev.empty != s_frame.empty) {
        mark(y0, y1, CONTENT_Y, CONTENT_Y + CONTENT_H);
        return;
    }
    if (s == DASH_SCREEN_STOCKS) {
        for (int i = 0; i < STOCK_ROWS; i++) {
            bool was = i < s_prev.u.stocks.n, is = i < s_frame.u.stocks.n;
            if (was != is || (is && memcmp(&s_prev.u.stocks.rows[i],
                                           &s_frame.u.stocks.rows[i],
                                           sizeof(stock_row_t)) != 0)) {
                int ry = CONTENT_Y + i * STOCK_ROW_H;
                mark(y0, y1, ry, ry + STOCK_ROW_H);
            }
        }
    } else if (memcmp(&s_prev.u, &s_frame.u, sizeof(s_frame.u)) != 0) {
        mark(y0, y1, CONTENT_Y, CONTENT_Y + CONTENT_H);
    }
}

// ---- drawing --------------------------------------------------------------

// Bottom nav: "<" left, dots center, ">" right.
static void draw_nav(uint16_t *buf, int sy0, int sh, dash_screen_t cur) {
    dash_fill(buf, sy0, sh, 0, DASH_H - NAV_H, DASH_W, DASH_H, DASH_BLACK);
    dash_fill(buf, sy0, sh, 0, DASH_H - NAV_H, DASH_W, DASH_H - NAV_H + 1,
              DASH_RULE);
    int cy = DASH_H - NAV_H + 6;
    dash_text(buf, sy0, sh, 14, cy, "<", 3, DASH_DIM, DASH_BLACK, true);
    dash_text(buf, sy0, sh, DASH_W - 14 - dash_text_w(">", 3), cy, ">", 3,
              DASH_DIM, DASH_BLACK, true);
    int dx = (DASH_W - DASH_SCREEN_COUNT * 14) / 2 + 3;
    for (int i = 0; i < DASH_SCREEN_COUNT; i++) {
        uint16_t c = (i == (int)cur) ? DASH_ACCENT : DASH_DOT;
        dash_fill(buf, sy0, sh, dx + i * 14, cy + 4, dx + i * 14 + 8, cy + 10, c);
    }
}

static void draw_top(uint16_t *buf, int sy0, int sh, const frame_t *f) {
    dash_fill(buf, sy0, sh, 0, 0, DASH_W, TOP_H, DASH_TOPBAR);
    dash_text(buf, sy0, sh, 12, 8, dash_screen_name(f->screen), 2, DASH_ACCENT,
              DASH_BLACK, true);
    if (f->right[0]) {
        dash_text_r(buf, sy0, sh, DASH_W - 12, 10, f->right, 1, DASH_DIM,
                    DASH_BLACK, true);
    }
    dash_fill(buf, sy0, sh, 0, TOP_H - 1, DASH_W, TOP_H, DASH_RULE);
}

static void draw_message(uint16_t *buf, int sy0, int sh, const char *msg) {
    dash_text_c(buf, sy0, sh, 0, DASH_W, CONTENT_Y + 60, msg, 2, DASH_DIM,
                DASH_BLACK, true);
}

static void draw_stocks(uint16_t *buf, int sy0, int sh, const frame_t *f) {
    if (f->empty) {
        draw_message(buf, sy0, sh, "waiting for data");
        return;
    }
    for (int i = 0; i < f->u.stocks.n; i++) {
        int y = CONTENT_Y + i * STOCK_ROW_H;
        if (y + STOCK_ROW_H <= sy0 || y >= sy0 + sh) continue;
        const stock_row_t *r = &f->u.stocks.rows[i];
        dash_fill(buf, sy0, sh, 8, y + 3, DASH_W - 8, y + STOCK_ROW_H - 1,
                  DASH_CARD);
        dash_text(buf, sy0, sh, 16, y + 8, r->symbol, 2, DASH_WHITE,
                  DASH_BLACK, true);
        dash_text_r(buf, sy0, sh, DASH_W - 16, y + 8, r->price, 2, DASH_WHITE,
                    DASH_BLACK, true);
        dash_text(buf, sy0, sh, 16, y + 24, r->chg, 1,
                  r->up ? DASH_GREEN : DASH_RED, DASH_BLACK, true);
        if (r->market[0]) {
            dash_text_r(buf, sy0, sh, DASH_W - 16, y + 24, r->market, 1,
                        DASH_DIM, DASH_BLACK, true);
        }
    }
}

static void draw_weather(uint16_t *buf, int sy0, int sh, const frame_t *f) {
    if (f->empty) {
        draw_message(buf, sy0, sh, "waiting for data");
        return;
    }
    const char *tmp = f->u.weather.temp;
    dash_text(buf, sy0, sh, 18, CONTENT_Y + 12, tmp, 6, DASH_WHITE,
              DASH_BLACK, true);
    dash_text(buf, sy0, sh, 18 + dash_text_w(tmp, 6) + 12, CONTENT_Y + 18,
              f->u.weather.desc, 2, DASH_ACCENT, DASH_BLACK, true);
    dash_text(buf, sy0, sh, 20, CONTENT_Y + 78, f->u.weather.feels, 2,
              DASH_DIM, DASH_BLACK, true);
    dash_text(buf, sy0, sh, 20, CONTENT_Y + 100, f->u.weather.hum, 1,
              DASH_DIM, DASH_BLACK, true);
    int fw = DASH_W / DASH_MAX_FORECAST;
    for (int i = 0; i < f->u.weather.n; i++) {
        int x0 = i * fw;
        dash_text_c(buf, sy0, sh, x0, x0 + fw, CONTENT_Y + 128,
                    f->u.weather.day[i], 1, DASH_DIM, DASH_BLACK, true);
        dash_text_c(buf, sy0, sh, x0, x0 + fw, CONTENT_Y + 142,
                    f->u.weather.hl[i], 2, DASH_WHITE, DASH_BLACK, true);
    }
}

static void draw_calendar(uint16_t *buf, int sy0, int sh, const frame_t *f) {
    if (f->empty) {
        draw_message(buf, sy0, sh, "no events");
        return;
    }
    for (int i = 0; i < f->u.cal.n; i++) {
        int y = CONTENT_Y + i * EVENT_ROW_H;
        if (y + EVENT_ROW_H <= sy0 || y >= sy0 + sh) continue;
        dash_fill(buf, sy0, sh, 8, y + 3, DASH_W - 8, y + EVENT_ROW_H - 1,
                  DASH_CARD);
        dash_text(buf, sy0, sh, 16, y + 8, f->u.cal.time[i], 1, DASH_ACCENT,
                  DASH_BLACK, true);
        dash_text(buf, sy0, sh, 16, y + 22, f->u.cal.title[i], 2, DASH_WHITE,
                  DASH_BLACK, true);
    }
}

void dash_screen_draw_strip(uint16_t *buf, int sy0, int sh) {
    for (int i = 0; i < DASH_W * sh; i++) buf[i] = DASH_BG;
    const frame_t *f = &s_frame;
    if (sy0 < TOP_H) draw_top(buf, sy0, sh, f);
    if (sy0 + sh > CONTENT_Y && sy0 < CONTENT_Y + CONTENT_H) {
        switch (f->screen) {
            case DASH_SCREEN_STOCKS: draw_stocks(buf, sy0, sh, f); break;
            case DASH_SCREEN_WEATHER: draw_weather(buf, sy0, sh, f); break;
            case DASH_SCREEN_CALENDAR: draw_calendar(buf, sy0, sh, f); break;
            default: break;
        }
    }
    if (sy0 + sh > DASH_H - NAV_H) draw_nav(buf, sy0, sh, f->screen);
}
