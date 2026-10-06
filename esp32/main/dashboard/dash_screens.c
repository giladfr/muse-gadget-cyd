/*
 * Dashboard screens implementation.
 *
 * Layout (320x240): a 32 px header (title, subtitle, clock, link/Wi-Fi), the
 * content area, and a 20 px footer with page dots. Text is anti-aliased Inter;
 * cards are rounded. Everything a screen shows is formatted into a frame_t
 * first, so drawing a strip is pure painting and two frames can be diffed.
 */
#include "dash_screens.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dash_clock.h"
#include "dash_draw.h"
#include "dash_icons.h"
#include "dash_store.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#define F_SMALL (&dash_font_small)
#define F_BODY (&dash_font_body)
#define F_LARGE (&dash_font_large)
#define F_HUGE (&dash_font_huge)

#define TOP_H DASH_CONTENT_Y0
#define CONTENT_Y DASH_CONTENT_Y0
#define CONTENT_H (DASH_CONTENT_Y1 - DASH_CONTENT_Y0)
#define MARGIN 10
#define RADIUS 8

// Up to 5 quotes / 4 events get roomy cards; longer lists switch to compact
// one-line rows so all 8 fit without paging.
#define STOCK_ROWS 5
#define EVENT_ROWS 4
#define MAX_ROWS 8
#define GAP 4

// Header geometry.
#define STATUS_X 292   // link dot + Wi-Fi bars, to the right edge
#define CLOCK_X1 (STATUS_X - 8)

// Sparkline: points kept per row, scaled to 0..255 of its box height.
#define SPARK_N 24
// A moved price tints its row for this long, fading out.
#define FLASH_MS 1500
#define FLASH_LEVELS 5

#define SCREEN_MESSAGE DASH_SCREEN_COUNT

typedef struct {
    char symbol[12];
    char price[16];
    char abs[16];     // "+3.10"
    char pct[12];     // "+1.74%"
    char market[12];
    bool up;
    uint8_t flash;    // 0..FLASH_LEVELS, tint strength
    int8_t flash_dir; // +1 up / -1 down
    uint8_t spark_n;
    uint8_t spark[SPARK_N];
} stock_row_t;

typedef enum { EV_NORMAL, EV_PAST, EV_NEXT } ev_state_t;

typedef struct {
    char time[12];
    char title[48];
    ev_state_t state;
    bool more;  // "+N more" row; the text is in title
} event_row_t;

typedef struct {
    bool valid;
    int screen;      // dash_screen_t or SCREEN_MESSAGE
    // Header.
    char title[24];
    char sub[32];    // dim subtitle after the title
    char clock[8];   // "5:45" / "17:45"; empty when the clock isn't set
    char ampm[4];
    bool status;     // draw link/Wi-Fi
    bool link;
    int wifi_bars;
    // Content.
    bool empty;
    char empty_msg[24];
    char empty_hint[48];
    bool compact;
    union {
        struct {
            stock_row_t rows[MAX_ROWS];
            int n;
        } stocks;
        struct {
            char temp[8];    // "78"
            int icon;
            char desc[24];
            char detail[48];
            char day[DASH_MAX_FORECAST][8];
            char hi[DASH_MAX_FORECAST][8];
            char lo[DASH_MAX_FORECAST][8];
            int day_icon[DASH_MAX_FORECAST];
            int n;
        } weather;
        struct {
            event_row_t rows[MAX_ROWS];
            int n;
        } cal;
        struct {
            char msg[48];
            char hint[48];
            int cx, cy;
            int progress;  // 0..100, -1 = no bar
        } message;
    } u;
} frame_t;

static frame_t s_frames[2];
static frame_t *s_frame = &s_frames[0];
static frame_t *s_prev = &s_frames[1];
static dash_store_t s_snap;  // static: keeps ~2 KB off the task stack
static bool s_animating;

const char *dash_screen_name(dash_screen_t s) {
    switch (s) {
        case DASH_SCREEN_STOCKS: return "Stocks";
        case DASH_SCREEN_WEATHER: return "Weather";
        case DASH_SCREEN_CALENDAR: return "Today";
        default: return "";
    }
}

bool dash_screen_animating(void) {
    return s_animating;
}

// ---- frame building -------------------------------------------------------

static void build_header(frame_t *f, const dash_status_t *st) {
    struct tm tm;
    if (dash_clock_local(&tm)) {
#if CONFIG_HOMEHUB_DASHBOARD_CLOCK_24H
        snprintf(f->clock, sizeof(f->clock), "%d:%02d", tm.tm_hour, tm.tm_min);
#else
        int h = tm.tm_hour % 12;
        snprintf(f->clock, sizeof(f->clock), "%d:%02d", h ? h : 12, tm.tm_min);
        snprintf(f->ampm, sizeof(f->ampm), "%s", tm.tm_hour < 12 ? "AM" : "PM");
#endif
    }
    if (st) {
        f->status = true;
        f->link = st->link;
        f->wifi_bars = st->wifi_bars;
    }
}

// Room for the subtitle: from after the title to before the clock.
static int sub_room(const frame_t *f) {
    int x0 = 14 + dash_text_w(F_BODY, f->title) + 8;
    int x1 = CLOCK_X1;
    if (f->clock[0]) {
        x1 -= dash_text_w(F_BODY, f->clock) + 8;
        if (f->ampm[0]) x1 -= dash_text_w(F_SMALL, f->ampm) + 3;
    }
    return x1 - x0;
}

static void set_sub(frame_t *f, const char *s) {
    dash_text_fit(F_SMALL, f->sub, sizeof(f->sub), s, sub_room(f));
}

static void build_stocks(frame_t *f, const dash_store_t *st, int64_t now_us) {
    int64_t age = dash_store_stocks_age_s(st);
    char sub[24];
    if (age == INT64_MAX) {
        snprintf(sub, sizeof(sub), "waiting");
    } else if (age < 90) {
        snprintf(sub, sizeof(sub), "live");
    } else if (age < 3600) {
        snprintf(sub, sizeof(sub), "%dm ago", (int)(age / 60));
    } else if (age < 48 * 3600) {
        snprintf(sub, sizeof(sub), "%dh ago", (int)(age / 3600));
    } else {
        snprintf(sub, sizeof(sub), "%dd ago", (int)(age / 86400));
    }
    set_sub(f, sub);
    int n = st->n_stocks < MAX_ROWS ? st->n_stocks : MAX_ROWS;
    f->u.stocks.n = n;
    f->empty = n == 0;
    if (f->empty) {
        snprintf(f->empty_msg, sizeof(f->empty_msg), "Waiting for quotes");
        snprintf(f->empty_hint, sizeof(f->empty_hint),
                 "Muse pushes them with dashboard.data");
    }
    f->compact = n > STOCK_ROWS;
    for (int i = 0; i < n; i++) {
        const dash_stock_t *s = &st->stocks[i];
        stock_row_t *r = &f->u.stocks.rows[i];
        snprintf(r->symbol, sizeof(r->symbol), "%s", s->symbol);
        snprintf(r->price, sizeof(r->price), "%.2f", s->price);
        snprintf(r->abs, sizeof(r->abs), "%+.2f", s->change);
        snprintf(r->pct, sizeof(r->pct), "%+.2f%%", s->change_pct);
        snprintf(r->market, sizeof(r->market), "%s", s->market);
        r->up = s->change >= 0;
        if (s->moved_us) {
            int64_t ms = (now_us - s->moved_us) / 1000;
            if (ms >= 0 && ms < FLASH_MS) {
                r->flash = (uint8_t)(FLASH_LEVELS - ms * FLASH_LEVELS / FLASH_MS);
                r->flash_dir = s->moved;
                s_animating = true;
            }
        }
        // Sparkline: the last SPARK_N prices, scaled to their own range.
        int hn = s->history_n, k0 = hn > SPARK_N ? hn - SPARK_N : 0;
        float lo = 1e30f, hi = -1e30f;
        for (int k = k0; k < hn; k++) {
            if (s->history[k] < lo) lo = s->history[k];
            if (s->history[k] > hi) hi = s->history[k];
        }
        r->spark_n = (uint8_t)(hn - k0);
        for (int k = k0; k < hn; k++) {
            float v = hi - lo > 1e-6f ? (s->history[k] - lo) / (hi - lo) : 0.5f;
            r->spark[k - k0] = (uint8_t)(v * 255.0f + 0.5f);
        }
    }
}

static void build_weather(frame_t *f, const dash_store_t *st) {
    f->empty = !st->weather_valid;
    if (f->empty) {
        snprintf(f->empty_msg, sizeof(f->empty_msg), "Waiting for weather");
        return;
    }
    const dash_weather_t *wx = &st->weather;
    set_sub(f, wx->location);
    snprintf(f->u.weather.temp, sizeof(f->u.weather.temp), "%d", wx->temp);
    f->u.weather.icon = dash_icon_for_code(wx->code);
    // Description fits between the left edge and the icon.
    dash_text_fit(F_LARGE, f->u.weather.desc, sizeof(f->u.weather.desc),
                  wx->desc, DASH_W - 2 * MARGIN - 70);
    char detail[64];
    snprintf(detail, sizeof(detail), "Feels %d" DASH_DEG "   %d%% hum   %d mph",
             wx->feels, wx->humidity, wx->wind);
    dash_text_fit(F_SMALL, f->u.weather.detail, sizeof(f->u.weather.detail),
                  detail, DASH_W - 2 * MARGIN - 8);
    int n = wx->forecast_n < DASH_MAX_FORECAST ? wx->forecast_n : DASH_MAX_FORECAST;
    f->u.weather.n = n;
    for (int i = 0; i < n; i++) {
        snprintf(f->u.weather.day[i], sizeof(f->u.weather.day[i]), "%s",
                 wx->forecast[i].day);
        snprintf(f->u.weather.hi[i], sizeof(f->u.weather.hi[i]),
                 "%d" DASH_DEG, wx->forecast[i].high);
        snprintf(f->u.weather.lo[i], sizeof(f->u.weather.lo[i]),
                 "%d" DASH_DEG, wx->forecast[i].low);
        f->u.weather.day_icon[i] = dash_icon_for_code(wx->forecast[i].code);
    }
}

// "5:45 PM", "5 pm", "17:45" -> minutes after midnight; -1 if not a time.
static int parse_minutes(const char *s) {
    while (*s == ' ') s++;
    if (!isdigit((unsigned char)*s)) return -1;
    char *end;
    long h = strtol(s, &end, 10), m = 0;
    if (*end == ':' || *end == '.') {
        if (!isdigit((unsigned char)end[1])) return -1;
        m = strtol(end + 1, &end, 10);
    }
    while (*end == ' ') end++;
    char c = (char)tolower((unsigned char)*end);
    if (c == 'p' && h < 12) h += 12;
    if (c == 'a' && h == 12) h = 0;
    if (h < 0 || h > 23 || m < 0 || m > 59) return -1;
    return (int)(h * 60 + m);
}

// Title column of an event row.
static int event_title_x(bool compact) {
    return compact ? MARGIN + 78 : MARGIN + 14;
}

static void build_calendar(frame_t *f, const dash_store_t *st) {
    set_sub(f, st->events_label);
    int n = st->n_events < MAX_ROWS ? st->n_events : MAX_ROWS;
    int total = st->n_events_total > n ? st->n_events_total : n;
    f->empty = n == 0;
    if (f->empty) {
        snprintf(f->empty_msg, sizeof(f->empty_msg), "Nothing scheduled");
        snprintf(f->empty_hint, sizeof(f->empty_hint), "Enjoy the free time");
    }
    f->compact = n > EVENT_ROWS;
    // More than fit: the last row says how many are left.
    int shown = total > n ? n - 1 : n;

    // Past/next only for a calendar pushed today, with the clock set.
    struct tm tm;
    int now_min = -1;
    if (dash_clock_local(&tm)
        && st->events_day == tm.tm_year * 1000 + tm.tm_yday) {
        now_min = tm.tm_hour * 60 + tm.tm_min;
    }
    bool next_found = false;
    int title_w = DASH_W - MARGIN - 10 - event_title_x(f->compact);
    for (int i = 0; i < shown; i++) {
        event_row_t *r = &f->u.cal.rows[i];
        snprintf(r->time, sizeof(r->time), "%s", st->events[i].time);
        dash_text_fit(F_BODY, r->title, sizeof(r->title), st->events[i].title,
                      title_w);
        int m = parse_minutes(st->events[i].time);
        if (now_min >= 0 && m >= 0) {
            // Without an end time, an event counts as over 30 min in.
            if (m + 30 <= now_min) {
                r->state = EV_PAST;
            } else if (!next_found) {
                r->state = EV_NEXT;
                next_found = true;
            }
        }
    }
    if (total > shown && shown < MAX_ROWS) {
        event_row_t *r = &f->u.cal.rows[shown];
        r->more = true;
        snprintf(r->title, sizeof(r->title), "+%d more", total - shown);
        shown++;
    }
    f->u.cal.n = shown;
}

// ---- row geometry -----------------------------------------------------------

static int rows_for(const frame_t *f) {
    if (f->screen == DASH_SCREEN_STOCKS) {
        return f->compact ? MAX_ROWS : STOCK_ROWS;
    }
    return f->compact ? MAX_ROWS : EVENT_ROWS;
}

// Card i spans [y, y + h) (the gap below is not part of it).
static void row_box(const frame_t *f, int i, int *y, int *h) {
    int n = rows_for(f);
    int pitch = (CONTENT_H - 4) / n;
    *y = CONTENT_Y + 2 + i * pitch;
    *h = pitch - (f->compact ? 2 : GAP);
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

// Mark the rows of a list screen that differ between the two frames.
static void diff_rows(int *y0, int *y1, int n_prev, int n_cur,
                      const void *prev_rows, const void *cur_rows,
                      size_t row_size) {
    int n = n_prev > n_cur ? n_prev : n_cur;
    for (int i = 0; i < n; i++) {
        bool was = i < n_prev, is = i < n_cur;
        const char *a = (const char *)prev_rows + i * row_size;
        const char *b = (const char *)cur_rows + i * row_size;
        if (was != is || (is && memcmp(a, b, row_size) != 0)) {
            int ry, rh;
            row_box(s_frame, i, &ry, &rh);
            mark(y0, y1, ry, ry + rh);
        }
    }
}

static void header_diff(int *y0, int *y1) {
    const frame_t *a = s_prev, *b = s_frame;
    if (strcmp(a->sub, b->sub) || strcmp(a->clock, b->clock)
        || strcmp(a->ampm, b->ampm) || a->link != b->link
        || a->wifi_bars != b->wifi_bars || a->status != b->status) {
        mark(y0, y1, 0, TOP_H);
    }
}

static void begin_frame(int screen, const char *title) {
    frame_t *t = s_prev;
    s_prev = s_frame;
    s_frame = t;
    memset(s_frame, 0, sizeof(*s_frame));
    s_frame->valid = true;
    s_frame->screen = screen;
    snprintf(s_frame->title, sizeof(s_frame->title), "%s", title);
}

void dash_screen_prepare(dash_screen_t s, const dash_status_t *st, bool full,
                         int *y0, int *y1) {
    dash_store_snapshot(&s_snap);
    begin_frame(s, dash_screen_name(s));
    s_animating = false;
    build_header(s_frame, st);
    switch (s) {
        case DASH_SCREEN_STOCKS:
            build_stocks(s_frame, &s_snap, esp_timer_get_time());
            break;
        case DASH_SCREEN_WEATHER: build_weather(s_frame, &s_snap); break;
        case DASH_SCREEN_CALENDAR: build_calendar(s_frame, &s_snap); break;
        default: break;
    }

    *y0 = *y1 = 0;
    if (full || !s_prev->valid || s_prev->screen != (int)s) {
        *y1 = DASH_H;
        return;
    }
    header_diff(y0, y1);
    if (s_prev->empty != s_frame->empty || s_prev->compact != s_frame->compact) {
        mark(y0, y1, CONTENT_Y, DASH_CONTENT_Y1);
        return;
    }
    if (s == DASH_SCREEN_STOCKS) {
        diff_rows(y0, y1, s_prev->u.stocks.n, s_frame->u.stocks.n,
                  s_prev->u.stocks.rows, s_frame->u.stocks.rows,
                  sizeof(stock_row_t));
    } else if (s == DASH_SCREEN_CALENDAR) {
        diff_rows(y0, y1, s_prev->u.cal.n, s_frame->u.cal.n,
                  s_prev->u.cal.rows, s_frame->u.cal.rows, sizeof(event_row_t));
    } else if (memcmp(&s_prev->u, &s_frame->u, sizeof(s_frame->u)) != 0) {
        mark(y0, y1, CONTENT_Y, DASH_CONTENT_Y1);
    }
}

void dash_screen_prepare_message(const char *title, const char *msg,
                                 const char *hint, int cx, int cy,
                                 int progress) {
    begin_frame(SCREEN_MESSAGE, title);
    // Not `valid`: whatever is prepared next repaints the whole screen.
    s_frame->valid = false;
    s_animating = false;
    snprintf(s_frame->u.message.msg, sizeof(s_frame->u.message.msg), "%s",
             msg ? msg : "");
    snprintf(s_frame->u.message.hint, sizeof(s_frame->u.message.hint), "%s",
             hint ? hint : "");
    s_frame->u.message.cx = cx;
    s_frame->u.message.cy = cy;
    s_frame->u.message.progress = progress;
}

// ---- drawing --------------------------------------------------------------

static void draw_status(uint16_t *buf, int sy0, int sh, const frame_t *f) {
    if (!f->status) return;
    dash_circle(buf, sy0, sh, STATUS_X + 3.5f, 16.5f, 3.2f,
                f->link ? DASH_UP : DASH_DOWN);
    for (int i = 0; i < 3; i++) {
        int x = STATUS_X + 11 + i * 5;
        int h = 4 + 3 * i;
        uint16_t c = i < f->wifi_bars ? DASH_TEXT : DASH_TEXT3;
        dash_round_rect(buf, sy0, sh, x, 22 - h, x + 3, 22, 1, c);
    }
}

static void draw_top(uint16_t *buf, int sy0, int sh, const frame_t *f) {
    if (sy0 >= TOP_H) return;
    int x = 14 + dash_text(buf, sy0, sh, F_BODY, 14, 7, f->title, DASH_TEXT);
    if (f->sub[0]) {
        uint16_t c = strcmp(f->sub, "live") == 0 ? DASH_UP : DASH_TEXT2;
        dash_text(buf, sy0, sh, F_SMALL, x + 8, 10, f->sub, c);
    }
    if (f->clock[0]) {
        int x1 = CLOCK_X1;
        if (f->ampm[0]) {
            x1 -= dash_text_w(F_SMALL, f->ampm);
            dash_text(buf, sy0, sh, F_SMALL, x1, 10, f->ampm, DASH_TEXT2);
            x1 -= 3;
        }
        dash_text_r(buf, sy0, sh, F_BODY, x1, 7, f->clock, DASH_TEXT);
    }
    draw_status(buf, sy0, sh, f);
}

// Footer: chevrons hinting at the tap zones, and page dots.
static void draw_nav(uint16_t *buf, int sy0, int sh, int cur) {
    if (sy0 + sh <= DASH_CONTENT_Y1) return;
    float cy = DASH_CONTENT_Y1 + 10.5f;
    dash_line(buf, sy0, sh, 20, cy - 4, 16, cy, 1.6f, DASH_TEXT3);
    dash_line(buf, sy0, sh, 16, cy, 20, cy + 4, 1.6f, DASH_TEXT3);
    dash_line(buf, sy0, sh, 300, cy - 4, 304, cy, 1.6f, DASH_TEXT3);
    dash_line(buf, sy0, sh, 304, cy, 300, cy + 4, 1.6f, DASH_TEXT3);
    int w = (DASH_SCREEN_COUNT - 1) * 12 + 18;
    int x = (DASH_W - w) / 2;
    for (int i = 0; i < DASH_SCREEN_COUNT; i++) {
        if (i == cur) {
            dash_round_rect(buf, sy0, sh, x, (int)cy - 3, x + 18, (int)cy + 3, 3,
                            DASH_ACCENT);
            x += 24;
        } else {
            dash_circle(buf, sy0, sh, x + 3, cy, 3, DASH_TEXT3);
            x += 12;
        }
    }
}

static void draw_empty(uint16_t *buf, int sy0, int sh, const frame_t *f) {
    int y = CONTENT_Y + CONTENT_H / 2 - 18;
    dash_text_c(buf, sy0, sh, F_BODY, 0, DASH_W, y, f->empty_msg, DASH_TEXT2);
    if (f->empty_hint[0]) {
        dash_text_c(buf, sy0, sh, F_SMALL, 0, DASH_W, y + 24, f->empty_hint,
                    DASH_TEXT3);
    }
}

// Pill with centred text.
static void pill(uint16_t *buf, int sy0, int sh, int x0, int y0, int x1, int y1,
                 uint16_t bg, const char *s, uint16_t fg) {
    dash_round_rect(buf, sy0, sh, x0, y0, x1, y1, (y1 - y0) / 2, bg);
    int ty = y0 + (y1 - y0 - dash_font_small.line_h) / 2;
    dash_text_c(buf, sy0, sh, F_SMALL, x0, x1, ty, s, fg);
}

static void sparkline(uint16_t *buf, int sy0, int sh, const stock_row_t *r,
                      int x0, int y0, int x1, int y1, uint16_t c) {
    if (r->spark_n < 2 || y1 + 2 <= sy0 || y0 - 2 >= sy0 + sh) return;
    float dx = (float)(x1 - x0) / (SPARK_N - 1);
    // Right-aligned: a short history grows in from the right.
    float xs = x1 - dx * (r->spark_n - 1);
    for (int k = 1; k < r->spark_n; k++) {
        float ya = y1 - r->spark[k - 1] * (y1 - y0) / 255.0f;
        float yb = y1 - r->spark[k] * (y1 - y0) / 255.0f;
        dash_line(buf, sy0, sh, xs + dx * (k - 1), ya, xs + dx * k, yb, 1.5f, c);
    }
}

static void draw_stocks(uint16_t *buf, int sy0, int sh, const frame_t *f) {
    if (f->empty) {
        draw_empty(buf, sy0, sh, f);
        return;
    }
    for (int i = 0; i < f->u.stocks.n; i++) {
        int y, h;
        row_box(f, i, &y, &h);
        if (y + h <= sy0 || y >= sy0 + sh) continue;
        const stock_row_t *r = &f->u.stocks.rows[i];
        uint16_t c = r->up ? DASH_UP : DASH_DOWN;
        uint16_t card = DASH_CARD;
        if (r->flash) {
            card = dash_mix(DASH_CARD, r->flash_dir > 0 ? DASH_UP : DASH_DOWN,
                            r->flash * 70 / FLASH_LEVELS);
        }
        dash_round_rect(buf, sy0, sh, MARGIN, y, DASH_W - MARGIN, y + h,
                        f->compact ? 6 : RADIUS, card);
        int bh = dash_font_body.line_h;
        if (f->compact) {
            int ty = y + (h - bh) / 2;
            dash_text(buf, sy0, sh, F_BODY, MARGIN + 10, ty, r->symbol, DASH_TEXT);
            sparkline(buf, sy0, sh, r, 86, y + 4, 150, y + h - 4, c);
            dash_text_r(buf, sy0, sh, F_BODY, 236, ty, r->price, DASH_TEXT);
            pill(buf, sy0, sh, 244, y + 3, DASH_W - MARGIN - 4, y + h - 3,
                 dash_mix(card, c, 50), r->pct, c);
            continue;
        }
        // Two lines, packed by their ink rather than their line boxes: the
        // body's caps sit 4 px into its box, the small line 16 px below.
        int top = y + (h - 30) / 2 - 2;
        dash_text(buf, sy0, sh, F_BODY, MARGIN + 10, top, r->symbol, DASH_TEXT);
        dash_text(buf, sy0, sh, F_SMALL, MARGIN + 10, top + 16, r->market,
                  DASH_TEXT3);
        sparkline(buf, sy0, sh, r, 86, y + 7, 150, y + h - 7, c);
        dash_text_r(buf, sy0, sh, F_BODY, 230, top, r->price, DASH_TEXT);
        dash_text_r(buf, sy0, sh, F_SMALL, 230, top + 16, r->abs, c);
        pill(buf, sy0, sh, 238, y + (h - 20) / 2, DASH_W - MARGIN - 6,
             y + (h + 20) / 2, dash_mix(card, c, 50), r->pct, c);
    }
}

static void draw_weather(uint16_t *buf, int sy0, int sh, const frame_t *f) {
    if (f->empty) {
        draw_empty(buf, sy0, sh, f);
        return;
    }
    // Hero: temperature, description and details; icon on the right.
    int y = CONTENT_Y + 2;
    int tw = dash_text(buf, sy0, sh, F_HUGE, MARGIN + 6, y - 6,
                       f->u.weather.temp, DASH_TEXT);
    dash_text(buf, sy0, sh, F_LARGE, MARGIN + 8 + tw, y + 4, DASH_DEG "F",
              DASH_TEXT2);
    dash_icon(buf, sy0, sh, DASH_W - MARGIN - 62, y + 2,
              (dash_icon_t)f->u.weather.icon, 56);
    dash_text(buf, sy0, sh, F_LARGE, MARGIN + 6, y + 58, f->u.weather.desc,
              DASH_ACCENT);
    dash_text(buf, sy0, sh, F_SMALL, MARGIN + 7, y + 84, f->u.weather.detail,
              DASH_TEXT2);
    // Forecast cards.
    int n = f->u.weather.n;
    if (!n) return;
    int cy0 = CONTENT_Y + 106, cy1 = DASH_CONTENT_Y1 - 2;
    int cw = (DASH_W - 2 * MARGIN - (n - 1) * 6) / n;
    for (int i = 0; i < n; i++) {
        int x0 = MARGIN + i * (cw + 6), x1 = x0 + cw;
        dash_round_rect(buf, sy0, sh, x0, cy0, x1, cy1, RADIUS,
                        i == 0 ? DASH_CARD2 : DASH_CARD);
        dash_text_c(buf, sy0, sh, F_SMALL, x0, x1, cy0 + 5, f->u.weather.day[i],
                    DASH_TEXT2);
        dash_icon(buf, sy0, sh, x0 + (cw - 24) / 2, cy0 + 21,
                  (dash_icon_t)f->u.weather.day_icon[i], 24);
        int hw = dash_text_w(F_BODY, f->u.weather.hi[i]);
        int lw = dash_text_w(F_SMALL, f->u.weather.lo[i]);
        int tx = x0 + (cw - hw - 4 - lw) / 2;
        dash_text(buf, sy0, sh, F_BODY, tx, cy0 + 49, f->u.weather.hi[i],
                  DASH_TEXT);
        dash_text(buf, sy0, sh, F_SMALL, tx + hw + 4, cy0 + 52,
                  f->u.weather.lo[i], DASH_TEXT3);
    }
}

static void draw_calendar(uint16_t *buf, int sy0, int sh, const frame_t *f) {
    if (f->empty) {
        draw_empty(buf, sy0, sh, f);
        return;
    }
    for (int i = 0; i < f->u.cal.n; i++) {
        int y, h;
        row_box(f, i, &y, &h);
        if (y + h <= sy0 || y >= sy0 + sh) continue;
        const event_row_t *r = &f->u.cal.rows[i];
        if (r->more) {
            dash_text_c(buf, sy0, sh, F_SMALL, 0, DASH_W,
                        y + (h - dash_font_small.line_h) / 2, r->title,
                        DASH_TEXT2);
            continue;
        }
        bool past = r->state == EV_PAST, next = r->state == EV_NEXT;
        uint16_t tc = past ? DASH_TEXT3 : next ? DASH_ACCENT : DASH_TEXT2;
        uint16_t fc = past ? DASH_TEXT3 : DASH_TEXT;
        dash_round_rect(buf, sy0, sh, MARGIN, y, DASH_W - MARGIN, y + h,
                        f->compact ? 6 : RADIUS, next ? DASH_CARD2 : DASH_CARD);
        if (next) {
            dash_round_rect(buf, sy0, sh, MARGIN + 4, y + 5, MARGIN + 7,
                            y + h - 5, 1, DASH_ACCENT);
        }
        int bh = dash_font_body.line_h;
        if (f->compact) {
            int ty = y + (h - bh) / 2;
            dash_text(buf, sy0, sh, F_SMALL, MARGIN + 14,
                      y + (h - dash_font_small.line_h) / 2, r->time, tc);
            dash_text(buf, sy0, sh, F_BODY, event_title_x(true), ty, r->title,
                      fc);
        } else {
            int top = y + (h - bh - dash_font_small.line_h - 2) / 2;
            dash_text(buf, sy0, sh, F_SMALL, event_title_x(false), top, r->time,
                      tc);
            if (next) {
                int tw = dash_text_w(F_SMALL, r->time);
                dash_text(buf, sy0, sh, F_SMALL, event_title_x(false) + tw + 8,
                          top, "NEXT", DASH_ACCENT);
            }
            dash_text(buf, sy0, sh, F_BODY, event_title_x(false),
                      top + dash_font_small.line_h + 2, r->title, fc);
        }
    }
}

static void draw_message_screen(uint16_t *buf, int sy0, int sh,
                                const frame_t *f) {
    const typeof(f->u.message) *m = &f->u.message;
    int y = m->cx >= 0 ? 104 : 96;
    dash_text_c(buf, sy0, sh, F_BODY, 0, DASH_W, y, m->msg, DASH_TEXT);
    if (m->hint[0]) {
        dash_text_c(buf, sy0, sh, F_SMALL, 0, DASH_W, y + 24, m->hint,
                    DASH_TEXT2);
    }
    if (m->progress >= 0) {
        int x0 = 40, x1 = DASH_W - 40, py = y + 52;
        dash_round_rect(buf, sy0, sh, x0, py, x1, py + 8, 4, DASH_CARD2);
        int w = (x1 - x0) * (m->progress > 100 ? 100 : m->progress) / 100;
        if (w > 0) {
            dash_round_rect(buf, sy0, sh, x0, py, x0 + (w < 8 ? 8 : w), py + 8, 4,
                            DASH_ACCENT);
        }
    }
    if (m->cx >= 0) {
        float cx = (float)m->cx, cy = (float)m->cy;
        dash_circle(buf, sy0, sh, cx, cy, 10, DASH_ACCENT);
        dash_circle(buf, sy0, sh, cx, cy, 8, DASH_BG);
        dash_line(buf, sy0, sh, cx - 16, cy, cx + 16, cy, 1.5f, DASH_TEXT);
        dash_line(buf, sy0, sh, cx, cy - 16, cx, cy + 16, 1.5f, DASH_TEXT);
        dash_circle(buf, sy0, sh, cx, cy, 2.5f, DASH_ACCENT);
    }
}

static void draw_frame(const frame_t *f, uint16_t *buf, int sy0, int sh) {
    for (int i = 0; i < DASH_W * sh; i++) buf[i] = DASH_BG;
    draw_top(buf, sy0, sh, f);
    if (f->screen == SCREEN_MESSAGE) {
        draw_message_screen(buf, sy0, sh, f);
        return;
    }
    if (sy0 + sh > CONTENT_Y && sy0 < DASH_CONTENT_Y1) {
        switch (f->screen) {
            case DASH_SCREEN_STOCKS: draw_stocks(buf, sy0, sh, f); break;
            case DASH_SCREEN_WEATHER: draw_weather(buf, sy0, sh, f); break;
            case DASH_SCREEN_CALENDAR: draw_calendar(buf, sy0, sh, f); break;
            default: break;
        }
    }
    draw_nav(buf, sy0, sh, f->screen);
}

void dash_screen_draw_strip(uint16_t *buf, int sy0, int sh) {
    draw_frame(s_frame, buf, sy0, sh);
}

void dash_screen_draw_strip_prev(uint16_t *buf, int sy0, int sh) {
    draw_frame(s_prev, buf, sy0, sh);
}
