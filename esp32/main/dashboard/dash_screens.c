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
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dash_cards.h"
#include "dash_clock.h"
#include "dash_draw.h"
#include "dash_icons.h"
#include "dash_splash.h"
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

#define SCREEN_MESSAGE (-1)
#define SCREEN_SPLASH (-2)
// A banner takes the header's place while it shows.
#define BANNER_H DASH_CONTENT_Y0
// Card button bar.
#define BUTTON_H 30
#define CARD_TEXT_LINES 4
// Analog clock face, and the Israel time line under it.
#define CLOCK_R 70
#define CLOCK_CX (DASH_W / 2)
#define CLOCK_CY (CONTENT_Y + 4 + CLOCK_R)
#define CLOCK_IL_Y (CLOCK_CY + CLOCK_R + 8)

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
    int screen;      // dash_screen_t, a card (DASH_SCREEN_COUNT + i), or
                     // SCREEN_MESSAGE
    int total;       // screens in the rotation, for the page dots
    bool has_banner;
    dash_banner_t banner;
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
            // Hand tips (and the second hand's tail), in screen pixels.
            float hour_x, hour_y, min_x, min_y, sec_x, sec_y, tail_x, tail_y;
            char israel[24];  // "Israel 14:30"
        } clock;
        struct {
            dash_card_t c;
            char lines[CARD_TEXT_LINES][64];
            int n_lines;
        } card;
        struct {
            int ms;            // since the splash began
            char version[24];  // "v1.5.4"
        } splash;
        struct {
            char msg[48];
            char hint[48];
            int cx, cy;
            int progress;  // 0..100, -1 = no bar
        } message;
    } u;
} frame_t;

static uint16_t tone_color(int tone);

static frame_t s_frames[2];
static frame_t *s_frame = &s_frames[0];
static frame_t *s_prev = &s_frames[1];
static dash_store_t s_snap;  // static: keeps ~2 KB off the task stack
static bool s_animating;

int dash_screen_total(void) {
    return DASH_SCREEN_COUNT + dash_cards_count();
}

const char *dash_screen_name(dash_screen_t s) {
    switch (s) {
        case DASH_SCREEN_STOCKS: return "Stocks";
        case DASH_SCREEN_WEATHER: return "Weather";
        case DASH_SCREEN_CALENDAR: return "Today";
        case DASH_SCREEN_CALENDAR_TOM: return "Tomorrow";
        case DASH_SCREEN_CLOCK: return "Clock";
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
#if CONFIG_HOMEHUB_DASHBOARD_QUOTES
        snprintf(f->empty_hint, sizeof(f->empty_hint), "Fetching from Nasdaq");
#else
        snprintf(f->empty_hint, sizeof(f->empty_hint),
                 "Muse pushes them with dashboard.data");
#endif
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

static void build_calendar(frame_t *f, const dash_store_t *st, bool tomorrow) {
    const dash_event_t *events = tomorrow ? st->events_tomorrow : st->events;
    int n_events = tomorrow ? st->n_events_tomorrow : st->n_events;
    int n_total = tomorrow ? st->n_events_tomorrow_total : st->n_events_total;
    const char *label = tomorrow ? st->events_tomorrow_label : st->events_label;
    int ev_day = tomorrow ? st->events_tomorrow_day : st->events_day;
    set_sub(f, label);
    int n = n_events < MAX_ROWS ? n_events : MAX_ROWS;
    int total = n_total > n ? n_total : n;
    f->empty = n == 0;
    if (f->empty) {
        snprintf(f->empty_msg, sizeof(f->empty_msg), "Nothing scheduled");
        snprintf(f->empty_hint, sizeof(f->empty_hint), "Enjoy the free time");
    }
    f->compact = n > EVENT_ROWS;
    // More than fit: the last row says how many are left.
    int shown = total > n ? n - 1 : n;

    // Past/next only for a calendar pushed today, with the clock set.
    // (Tomorrow's events are all in the future.)
    struct tm tm;
    int now_min = -1;
    if (!tomorrow && dash_clock_local(&tm)
        && ev_day == tm.tm_year * 1000 + tm.tm_yday) {
        now_min = tm.tm_hour * 60 + tm.tm_min;
    }
    bool next_found = false;
    int title_w = DASH_W - MARGIN - 10 - event_title_x(f->compact);
    for (int i = 0; i < shown; i++) {
        event_row_t *r = &f->u.cal.rows[i];
        // events[i].time is char[16], r->time is char[12]; copy safely.
        size_t copy_n = sizeof(r->time) - 1;
        if (copy_n > sizeof(events[i].time) - 1) copy_n = sizeof(events[i].time) - 1;
        memcpy(r->time, events[i].time, copy_n);
        r->time[copy_n] = '\0';
        dash_text_fit(F_BODY, r->title, sizeof(r->title), events[i].title,
                      title_w);
        int m = parse_minutes(events[i].time);
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

// Hand angle in radians, clockwise from 12, for `turns` of a full circle.
static void hand(float turns, float len, float *x, float *y) {
    float a = turns * 6.2831853f;
    *x = CLOCK_CX + len * sinf(a);
    *y = CLOCK_CY - len * cosf(a);
}

static void build_clock(frame_t *f) {
    struct tm tm, il;
    if (!dash_clock_local(&tm)) {
        f->empty = true;
        snprintf(f->empty_msg, sizeof(f->empty_msg), "Clock not set");
        snprintf(f->empty_hint, sizeof(f->empty_hint), "Waiting for network time");
        return;
    }
    static const char *const days[] = {"Sunday", "Monday", "Tuesday", "Wednesday",
                                       "Thursday", "Friday", "Saturday"};
    static const char *const months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                         "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    char date[32];
    snprintf(date, sizeof(date), "%s, %s %d", days[tm.tm_wday % 7],
             months[tm.tm_mon % 12], tm.tm_mday);
    set_sub(f, date);
    // Whole seconds only: the frame changes once a second, and only the
    // hands' rows are repainted (see dash_screen_prepare).
    float sec = tm.tm_sec / 60.0f;
    float min = (tm.tm_min + sec) / 60.0f;
    float hr = ((tm.tm_hour % 12) + min) / 12.0f;
    hand(hr, CLOCK_R * 0.5f, &f->u.clock.hour_x, &f->u.clock.hour_y);
    hand(min, CLOCK_R * 0.78f, &f->u.clock.min_x, &f->u.clock.min_y);
    hand(sec, CLOCK_R * 0.86f, &f->u.clock.sec_x, &f->u.clock.sec_y);
    hand(sec + 0.5f, CLOCK_R * 0.2f, &f->u.clock.tail_x, &f->u.clock.tail_y);
    if (dash_clock_israel(&il)) {
        snprintf(f->u.clock.israel, sizeof(f->u.clock.israel), "Israel %02d:%02d",
                 il.tm_hour, il.tm_min);
    }
}

// Rows [y0, y1) the hands cover, including their width.
static void clock_hand_rows(const frame_t *f, int *y0, int *y1) {
    const float ys[] = {f->u.clock.hour_y, f->u.clock.min_y, f->u.clock.sec_y,
                        f->u.clock.tail_y, CLOCK_CY};
    float lo = ys[0], hi = ys[0];
    for (size_t i = 1; i < sizeof(ys) / sizeof(ys[0]); i++) {
        if (ys[i] < lo) lo = ys[i];
        if (ys[i] > hi) hi = ys[i];
    }
    *y0 = (int)lo - 6;  // half the widest hand, the hub, anti-aliasing
    *y1 = (int)hi + 7;
}

// Word-wrap `text` into up to CARD_TEXT_LINES lines of at most max_w pixels;
// the last line gets "..." if the text doesn't fit.
static int wrap(const dash_font_t *font, const char *text, int max_w,
                char lines[][64]) {
    int n = 0;
    const char *p = text;
    while (*p && n < CARD_TEXT_LINES) {
        while (*p == ' ') p++;
        // Longest prefix that fits, broken at a space when possible.
        char line[64];
        int len = 0, brk = -1;
        while (p[len] && p[len] != '\n' && len < 63) {
            line[len] = p[len];
            line[len + 1] = '\0';
            if (dash_text_w(font, line) > max_w) break;
            if (p[len] == ' ') brk = len;
            len++;
        }
        bool more = p[len] && p[len] != '\n';
        if (more && brk > 0) len = brk;
        bool last = n == CARD_TEXT_LINES - 1;
        memcpy(line, p, (size_t)len);
        line[len] = '\0';
        if (last && (more || p[len] == '\n')) {
            char rest[sizeof(((dash_card_t *)0)->text)];
            snprintf(rest, sizeof(rest), "%s", p);
            dash_text_fit(font, line, sizeof(line), rest, max_w);
        }
        memcpy(lines[n++], line, sizeof(line));
        p += len;
        if (*p == '\n') p++;
    }
    return n;
}

static void build_card(frame_t *f, int index) {
    if (!dash_cards_get(index, &f->u.card.c)) {
        f->empty = true;
        snprintf(f->empty_msg, sizeof(f->empty_msg), "Card removed");
        return;
    }
    const dash_card_t *c = &f->u.card.c;
    snprintf(f->title, sizeof(f->title), "%s", c->title);
    set_sub(f, c->status[0] ? c->status : c->sub);
    if (c->text[0]) {
        f->u.card.n_lines = wrap(F_BODY, c->text, DASH_W - 2 * MARGIN - 20,
                                 f->u.card.lines);
    }
    f->empty = !c->text[0] && !c->n_rows && !c->n_buttons;
    if (f->empty) snprintf(f->empty_msg, sizeof(f->empty_msg), "Empty card");
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

void dash_screen_prepare(int s, const dash_status_t *st, bool full, int *y0,
                         int *y1) {
    dash_store_snapshot(&s_snap);
    begin_frame(s, s < DASH_SCREEN_COUNT ? dash_screen_name((dash_screen_t)s) : "");
    s_animating = false;
    s_frame->total = dash_screen_total();
    build_header(s_frame, st);
    switch (s) {
        case DASH_SCREEN_STOCKS:
            build_stocks(s_frame, &s_snap, esp_timer_get_time());
            break;
        case DASH_SCREEN_WEATHER: build_weather(s_frame, &s_snap); break;
        case DASH_SCREEN_CALENDAR: build_calendar(s_frame, &s_snap, false); break;
        case DASH_SCREEN_CALENDAR_TOM: build_calendar(s_frame, &s_snap, true); break;
        case DASH_SCREEN_CLOCK: build_clock(s_frame); break;
        default: build_card(s_frame, s - DASH_SCREEN_COUNT); break;
    }
    s_frame->has_banner = dash_banner_current(&s_frame->banner);

    *y0 = *y1 = 0;
    if (full || !s_prev->valid || s_prev->screen != s
        || s_prev->total != s_frame->total) {
        *y1 = DASH_H;
        return;
    }
    if (s_prev->has_banner != s_frame->has_banner
        || (s_frame->has_banner
            && memcmp(&s_prev->banner, &s_frame->banner, sizeof(dash_banner_t)))) {
        mark(y0, y1, 0, BANNER_H);
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
    } else if (s == DASH_SCREEN_CALENDAR || s == DASH_SCREEN_CALENDAR_TOM) {
        diff_rows(y0, y1, s_prev->u.cal.n, s_frame->u.cal.n,
                  s_prev->u.cal.rows, s_frame->u.cal.rows, sizeof(event_row_t));
    } else if (s == DASH_SCREEN_CLOCK) {
        // Only the rows the hands sweep (old and new positions), and the
        // Israel line when its minute turns.
        if (memcmp(&s_prev->u.clock, &s_frame->u.clock,
                   offsetof(typeof(s_frame->u.clock), israel))) {
            int a0, a1, b0, b1;
            clock_hand_rows(s_prev, &a0, &a1);
            clock_hand_rows(s_frame, &b0, &b1);
            mark(y0, y1, a0 < b0 ? a0 : b0, a1 > b1 ? a1 : b1);
        }
        if (strcmp(s_prev->u.clock.israel, s_frame->u.clock.israel)) {
            mark(y0, y1, CLOCK_IL_Y, CLOCK_IL_Y + dash_font_body.line_h);
        }
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

void dash_screen_prepare_splash(int ms, const char *version) {
    begin_frame(SCREEN_SPLASH, "");
    s_frame->valid = false;  // the first dashboard frame repaints everything
    s_animating = false;
    s_frame->u.splash.ms = ms;
    snprintf(s_frame->u.splash.version, sizeof(s_frame->u.splash.version), "%s",
             version ? version : "");
    dash_splash_seek(ms * DASH_SPLASH_FPS / 1000);
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
    uint16_t title_c = f->screen >= DASH_SCREEN_COUNT
                           ? tone_color(f->u.card.c.tone) : DASH_TEXT;
    int x = 14 + dash_text(buf, sy0, sh, F_BODY, 14, 7, f->title, title_c);
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
static void draw_nav(uint16_t *buf, int sy0, int sh, int cur, int total) {
    if (sy0 + sh <= DASH_CONTENT_Y1) return;
    float cy = DASH_CONTENT_Y1 + 10.5f;
    dash_line(buf, sy0, sh, 20, cy - 4, 16, cy, 1.6f, DASH_TEXT3);
    dash_line(buf, sy0, sh, 16, cy, 20, cy + 4, 1.6f, DASH_TEXT3);
    dash_line(buf, sy0, sh, 300, cy - 4, 304, cy, 1.6f, DASH_TEXT3);
    dash_line(buf, sy0, sh, 304, cy, 300, cy + 4, 1.6f, DASH_TEXT3);
    int w = (total - 1) * 12 + 18;
    int x = (DASH_W - w) / 2;
    for (int i = 0; i < total; i++) {
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

static void draw_clock(uint16_t *buf, int sy0, int sh, const frame_t *f) {
    if (f->empty) {
        draw_empty(buf, sy0, sh, f);
        return;
    }
    const float cx = CLOCK_CX, cy = CLOCK_CY, r = CLOCK_R;
    if (sy0 < cy + r + 2 && sy0 + sh > cy - r - 2) {
        // Face: a raised rim and the card colour inside.
        dash_circle(buf, sy0, sh, cx, cy, r, DASH_CARD2);
        dash_circle(buf, sy0, sh, cx, cy, r - 2, DASH_CARD);
        // Ticks: 12 hour marks, the quarters longer, and faint minutes.
        // dash_line returns at once for a strip it doesn't touch.
        static float tick_sin[60], tick_cos[60];
        if (tick_cos[0] == 0) {
            for (int i = 0; i < 60; i++) {
                tick_sin[i] = sinf(i * 6.2831853f / 60);
                tick_cos[i] = cosf(i * 6.2831853f / 60);
            }
        }
        for (int i = 0; i < 60; i++) {
            bool hour = i % 5 == 0, quarter = i % 15 == 0;
            float r0 = r - (quarter ? 15 : hour ? 11 : 7), r1 = r - 6;
            dash_line(buf, sy0, sh, cx + r0 * tick_sin[i], cy - r0 * tick_cos[i],
                      cx + r1 * tick_sin[i], cy - r1 * tick_cos[i],
                      hour ? 2.5f : 1.0f, hour ? DASH_TEXT2 : DASH_TEXT3);
        }
        // Hands: hour and minute, then the second hand with a short tail.
        dash_line(buf, sy0, sh, cx, cy, f->u.clock.hour_x, f->u.clock.hour_y,
                  5.0f, DASH_TEXT);
        dash_line(buf, sy0, sh, cx, cy, f->u.clock.min_x, f->u.clock.min_y,
                  3.0f, DASH_TEXT);
        dash_line(buf, sy0, sh, f->u.clock.tail_x, f->u.clock.tail_y,
                  f->u.clock.sec_x, f->u.clock.sec_y, 1.5f, DASH_ACCENT);
        dash_circle(buf, sy0, sh, cx, cy, 4.5f, DASH_ACCENT);
        dash_circle(buf, sy0, sh, cx, cy, 1.5f, DASH_CARD);
    }
    if (f->u.clock.israel[0]) {
        dash_text_c(buf, sy0, sh, F_BODY, 0, DASH_W, CLOCK_IL_Y, f->u.clock.israel,
                    DASH_TEXT2);
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

// ---- cards and banners ----------------------------------------------------

static uint16_t tone_color(int tone) {
    switch (tone) {
        case DASH_TONE_UP: return DASH_UP;
        case DASH_TONE_DOWN: return DASH_DOWN;
        case DASH_TONE_ACCENT: return DASH_ACCENT;
        case DASH_TONE_BLUE: return DASH_BLUE;
        case DASH_TONE_DIM: return DASH_TEXT2;
        default: return DASH_TEXT;
    }
}

// Where a card's parts go: wrapped text, then rows, then the button bar.
typedef struct {
    int rows_y0, row_pitch, n_rows;
    bool compact;
    int btn_y0;
} card_layout_t;

static void card_layout(const frame_t *f, card_layout_t *l) {
    const dash_card_t *c = &f->u.card.c;
    int y = CONTENT_Y + 4;
    if (f->u.card.n_lines) y += f->u.card.n_lines * 20 + 8;
    int y1 = DASH_CONTENT_Y1 - 2;
    l->btn_y0 = y1 - BUTTON_H;
    if (c->n_buttons) y1 = l->btn_y0 - 6;
    l->rows_y0 = y;
    l->n_rows = c->n_rows;
    l->row_pitch = l->n_rows ? (y1 - y) / l->n_rows : 0;
    if (l->row_pitch > 40) l->row_pitch = 40;
    // Too many rows for the room left: drop what doesn't fit.
    while (l->n_rows && l->row_pitch < 22) {
        l->n_rows--;
        l->row_pitch = l->n_rows ? (y1 - y) / l->n_rows : 0;
    }
    l->compact = l->row_pitch < 34;
}

static void button_rect(const frame_t *f, int i, int *x0, int *x1, int *y0,
                        int *y1) {
    card_layout_t l;
    card_layout(f, &l);
    int n = f->u.card.c.n_buttons;
    int gap = 8, w = (DASH_W - 2 * MARGIN - (n - 1) * gap) / n;
    *x0 = MARGIN + i * (w + gap);
    *x1 = *x0 + w;
    *y0 = l.btn_y0;
    *y1 = l.btn_y0 + BUTTON_H;
}

static void card_spark(uint16_t *buf, int sy0, int sh, const dash_card_row_t *r,
                       int x0, int y0, int x1, int y1, uint16_t c) {
    if (r->spark_n < 2) return;
    float dx = (float)(x1 - x0) / (r->spark_n - 1);
    for (int k = 1; k < r->spark_n; k++) {
        dash_line(buf, sy0, sh, x0 + dx * (k - 1),
                  y1 - r->spark[k - 1] * (y1 - y0) / 255.0f, x0 + dx * k,
                  y1 - r->spark[k] * (y1 - y0) / 255.0f, 1.5f, c);
    }
}

static void draw_card(uint16_t *buf, int sy0, int sh, const frame_t *f) {
    if (f->empty) {
        draw_empty(buf, sy0, sh, f);
        return;
    }
    const dash_card_t *c = &f->u.card.c;
    for (int i = 0; i < f->u.card.n_lines; i++) {
        dash_text(buf, sy0, sh, F_BODY, MARGIN + 6, CONTENT_Y + 4 + i * 20,
                  f->u.card.lines[i], DASH_TEXT);
    }
    card_layout_t l;
    card_layout(f, &l);
    for (int i = 0; i < l.n_rows; i++) {
        const dash_card_row_t *r = &c->rows[i];
        int y = l.rows_y0 + i * l.row_pitch, h = l.row_pitch - (l.compact ? 2 : 4);
        if (y + h <= sy0 || y >= sy0 + sh) continue;
        uint16_t vc = tone_color(r->tone);
        uint16_t lc = r->tone == DASH_TONE_DEFAULT ? DASH_UP : vc;  // bar/spark
        dash_round_rect(buf, sy0, sh, MARGIN, y, DASH_W - MARGIN, y + h,
                        l.compact ? 6 : RADIUS, DASH_CARD);
        int bh = dash_font_body.line_h;
        int ty = l.compact ? y + (h - bh) / 2 : y + (h - 30) / 2 - 2;
        dash_text(buf, sy0, sh, F_BODY, MARGIN + 10, ty, r->label, DASH_TEXT);
        if (!l.compact && r->detail[0]) {
            dash_text(buf, sy0, sh, F_SMALL, MARGIN + 10, ty + 16, r->detail,
                      DASH_TEXT2);
        }
        dash_text_r(buf, sy0, sh, F_BODY, DASH_W - MARGIN - 10, ty, r->value, vc);
        card_spark(buf, sy0, sh, r, 150, y + 6, 210, y + h - 6, lc);
        if (r->progress >= 0) {
            // Under the value, on the right, clear of the label and detail.
            int bx0 = DASH_W / 2 + 10, bx1 = DASH_W - MARGIN - 10;
            int by = l.compact ? y + h - 5 : ty + 24;
            dash_round_rect(buf, sy0, sh, bx0, by, bx1, by + 3, 1, DASH_CARD2);
            int w = (bx1 - bx0) * r->progress / 100;
            if (w > 2) dash_round_rect(buf, sy0, sh, bx0, by, bx0 + w, by + 3, 1, lc);
        }
    }
    for (int i = 0; i < c->n_buttons; i++) {
        int x0, x1, y0, y1;
        button_rect(f, i, &x0, &x1, &y0, &y1);
        if (y1 <= sy0 || y0 >= sy0 + sh) continue;
        bool pressed = c->pressed == i;
        dash_round_rect(buf, sy0, sh, x0, y0, x1, y1, BUTTON_H / 2,
                        pressed ? DASH_ACCENT : DASH_CARD2);
        dash_text_c(buf, sy0, sh, F_BODY, x0, x1,
                    y0 + (BUTTON_H - dash_font_body.line_h) / 2,
                    c->buttons[i].label, pressed ? DASH_BG : DASH_TEXT);
    }
}

static void draw_banner(uint16_t *buf, int sy0, int sh, const frame_t *f) {
    if (!f->has_banner || sy0 >= BANNER_H) return;
    const dash_banner_t *b = &f->banner;
    uint16_t tc = tone_color(b->tone);
    dash_fill(buf, sy0, sh, 0, 0, DASH_W, BANNER_H, DASH_BG);
    dash_round_rect(buf, sy0, sh, 4, 2, DASH_W - 4, BANNER_H - 2, 8,
                    dash_mix(DASH_CARD2, tc, 45));
    dash_round_rect(buf, sy0, sh, 10, 8, 13, BANNER_H - 8, 1, tc);
    // One line: the headline, then as much of the detail as fits.
    char line[72];
    int x = 20, x1 = DASH_W - 12;
    dash_text_fit(F_BODY, line, sizeof(line), b->text, x1 - x);
    x += dash_text(buf, sy0, sh, F_BODY, x, 6, line, DASH_TEXT) + 8;
    if (b->detail[0] && x1 - x > 40) {
        dash_text_fit(F_SMALL, line, sizeof(line), b->detail, x1 - x);
        dash_text(buf, sy0, sh, F_SMALL, x, 9, line, DASH_TEXT2);
    }
}

int dash_screen_hit(int x, int y) {
    const frame_t *f = s_frame;
    if (f->has_banner && y < BANNER_H) return DASH_HIT_BANNER;
    if (f->screen < DASH_SCREEN_COUNT || f->empty) return DASH_HIT_NONE;
    for (int i = 0; i < f->u.card.c.n_buttons; i++) {
        int x0, x1, y0, y1;
        button_rect(f, i, &x0, &x1, &y0, &y1);
        // A little slack around each button for fingers.
        if (x >= x0 - 3 && x < x1 + 3 && y >= y0 - 6 && y < y1 + 6) return i;
    }
    return DASH_HIT_NONE;
}

bool dash_screen_card(const char **id, const char **title,
                      const dash_card_button_t **buttons, int *n) {
    const frame_t *f = s_frame;
    if (f->screen < DASH_SCREEN_COUNT || f->empty) return false;
    *id = f->u.card.c.id;
    *title = f->u.card.c.title;
    *buttons = f->u.card.c.buttons;
    *n = f->u.card.c.n_buttons;
    return true;
}

bool dash_screen_banner(dash_banner_t *out) {
    if (!s_frame->has_banner) return false;
    *out = s_frame->banner;
    return true;
}

static float clamp01(float v) {
    return v < 0 ? 0 : v > 1 ? 1 : v;
}

// Boot splash: the mascot pops up and cheers, "Muse Dashboard" rises in under
// it, then the version, and three dots pulse while the Link comes up.
#define SPLASH_AV_Y (-12)
#define SPLASH_TITLE_Y 170
static void draw_splash(uint16_t *buf, int sy0, int sh, const frame_t *f) {
    const int ms = f->u.splash.ms;
    dash_splash_draw(buf, sy0, sh, (DASH_W - DASH_SPLASH_SIZE) / 2, SPLASH_AV_Y);
    // Title: fades in and rises 10 px (ease out) from 0.8 s.
    float a = clamp01((ms - 800) / 500.0f);
    if (a > 0) {
        float e = 1 - (1 - a) * (1 - a) * (1 - a);
        int y = SPLASH_TITLE_Y + (int)((1 - e) * 10);
        const char *w1 = "Muse", *w2 = " Dashboard";
        int w = dash_text_w(F_LARGE, w1) + dash_text_w(F_LARGE, w2);
        int x = (DASH_W - w) / 2;
        int alpha = (int)(e * 255);
        x += dash_text(buf, sy0, sh, F_LARGE, x, y, w1,
                       dash_mix(DASH_BG, DASH_ACCENT, alpha));
        dash_text(buf, sy0, sh, F_LARGE, x, y, w2, dash_mix(DASH_BG, DASH_TEXT, alpha));
    }
    float v = clamp01((ms - 1300) / 500.0f);
    if (v > 0 && f->u.splash.version[0]) {
        dash_text_c(buf, sy0, sh, F_SMALL, 0, DASH_W, SPLASH_TITLE_Y + 29,
                    f->u.splash.version, dash_mix(DASH_BG, DASH_TEXT3, (int)(v * 255)));
    }
    // Loading dots, a wave running left to right.
    float d = clamp01((ms - 1500) / 400.0f);
    if (d > 0 && sy0 < 236 && sy0 + sh > 222) {
        for (int i = 0; i < 3; i++) {
            float ph = sinf((ms / 1000.0f) * 7.0f - i * 0.9f) * 0.5f + 0.5f;
            uint16_t c = dash_mix(DASH_TEXT3, DASH_ACCENT, (int)(ph * 255));
            dash_circle(buf, sy0, sh, DASH_W / 2.0f + (i - 1) * 14, 229 - ph * 2,
                        2.2f + ph * 1.0f, dash_mix(DASH_BG, c, (int)(d * 255)));
        }
    }
}

static void draw_frame(const frame_t *f, uint16_t *buf, int sy0, int sh) {
    for (int i = 0; i < DASH_W * sh; i++) buf[i] = DASH_BG;
    draw_top(buf, sy0, sh, f);
    if (f->screen == SCREEN_MESSAGE) {
        draw_message_screen(buf, sy0, sh, f);
        return;
    }
    if (f->screen == SCREEN_SPLASH) {
        draw_splash(buf, sy0, sh, f);
        return;
    }
    if (sy0 + sh > CONTENT_Y && sy0 < DASH_CONTENT_Y1) {
        switch (f->screen) {
            case DASH_SCREEN_STOCKS: draw_stocks(buf, sy0, sh, f); break;
            case DASH_SCREEN_WEATHER: draw_weather(buf, sy0, sh, f); break;
            case DASH_SCREEN_CALENDAR:
            case DASH_SCREEN_CALENDAR_TOM: draw_calendar(buf, sy0, sh, f); break;
            case DASH_SCREEN_CLOCK: draw_clock(buf, sy0, sh, f); break;
            default: draw_card(buf, sy0, sh, f); break;
        }
    }
    draw_nav(buf, sy0, sh, f->screen, f->total);
    draw_banner(buf, sy0, sh, f);
}

void dash_screen_draw_strip(uint16_t *buf, int sy0, int sh) {
    draw_frame(s_frame, buf, sy0, sh);
}

void dash_screen_draw_strip_prev(uint16_t *buf, int sy0, int sh) {
    draw_frame(s_prev, buf, sy0, sh);
}
