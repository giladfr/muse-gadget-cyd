/*
 * Muse-defined content for the dashboard, so new widgets need no firmware:
 *
 *   cards    extra screens after the built-in ones (dashboard.card): a title,
 *            optional wrapped text, rows (label / value / detail, colour,
 *            progress bar or sparkline) and up to three buttons whose taps go
 *            back to Muse (dash_events).
 *   banners  short notifications over whatever screen is up
 *            (dashboard.notify), one at a time from a small queue; a tap
 *            dismisses one, or opens the card it links to.
 *
 * Kept in RAM: after a reboot Muse pushes them again.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DASH_MAX_CARDS 4
#define DASH_CARD_ROWS 5
#define DASH_CARD_BUTTONS 3
#define DASH_CARD_SPARK 24

// Row/accent colours by name.
typedef enum {
    DASH_TONE_DEFAULT,
    DASH_TONE_UP,      // green
    DASH_TONE_DOWN,    // red
    DASH_TONE_ACCENT,  // amber
    DASH_TONE_BLUE,
    DASH_TONE_DIM,
} dash_tone_t;

typedef struct {
    char label[24];
    char value[20];
    char detail[32];
    uint8_t tone;            // dash_tone_t for the value (and bar/sparkline)
    int8_t progress;         // 0..100, -1 = none
    uint8_t spark_n;
    uint8_t spark[DASH_CARD_SPARK];  // 0..255, oldest first
} dash_card_row_t;

typedef struct {
    char id[16];
    char label[16];
    char say[96];            // what the tap tells Muse ("" = describe the tap)
} dash_card_button_t;

typedef struct {
    char id[16];
    char title[20];
    char sub[32];            // subtitle (replaced by the tap status after one)
    char text[200];          // optional paragraph, wrapped
    uint8_t tone;            // accent for the title bar
    dash_card_row_t rows[DASH_CARD_ROWS];
    int n_rows;
    dash_card_button_t buttons[DASH_CARD_BUTTONS];
    int n_buttons;
    int8_t pressed;          // button last tapped, -1 none
    char status[32];         // "Sent to Muse", "Queued", ...
    int64_t expires_us;      // 0 = stays until removed
} dash_card_t;

typedef struct {
    bool active;
    char text[72];
    char detail[72];
    uint8_t tone;            // info=blue, success=up, warning=accent, alert=down
    char card[16];           // tap opens this card ("" = tap dismisses)
    int64_t expires_us;
} dash_banner_t;

// dashboard.card: {id, title, sub?, text?, tone?, rows?, buttons?, ttl_s?}
// creates or replaces card `id`; {id, remove: true} deletes it. *show is set
// when the card asked to be shown now ("show": true). Returns false with
// *err on bad input.
bool dash_cards_set(const cJSON *card, bool *show, const char **err);
bool dash_cards_remove(const char *id);
int dash_cards_count(void);
// Copy card `index` (0 = first extra screen). False if there is none.
bool dash_cards_get(int index, dash_card_t *out);
int dash_cards_find(const char *id);  // index, -1 if absent
// Record a tap on a card button (for its "pressed" look and status line).
void dash_cards_set_pressed(const char *id, int button, const char *status);
void dash_cards_set_status(const char *id, const char *status);

// dashboard.notify: {text, detail?, level?: info|success|warning|alert,
// card?, ttl_s?}. Queues a banner (oldest dropped when full).
bool dash_banner_push(const cJSON *n, const char **err);
// The banner to show now (expired ones are dropped); false if none.
bool dash_banner_current(dash_banner_t *out);
void dash_banner_dismiss(void);

// Drop expired cards and banners. True if anything visible changed.
bool dash_cards_tick(void);

#ifdef __cplusplus
}
#endif
