/*
 * Muse-defined cards and banners.
 */
#include "dash_cards.h"

#include <string.h>
#include <strings.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "dash.cards";

#define BANNER_QUEUE 4
#define BANNER_TTL_S 20
#define MAX_TTL_S (7 * 24 * 3600)

static dash_card_t s_cards[DASH_MAX_CARDS];
static int s_n_cards;
static dash_banner_t s_banners[BANNER_QUEUE];  // [0] is showing
static int s_n_banners;
static SemaphoreHandle_t s_lock;

static void lock(void) {
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void unlock(void) {
    xSemaphoreGive(s_lock);
}

static void str(char *dst, size_t n, const cJSON *obj, const char *key) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(v) && v->valuestring) {
        strncpy(dst, v->valuestring, n - 1);
        dst[n - 1] = '\0';
    } else if (cJSON_IsNumber(v)) {
        // Let Muse send numbers for values without formatting them first.
        snprintf(dst, n, "%g", v->valuedouble);
    } else {
        dst[0] = '\0';
    }
}

static uint8_t tone(const cJSON *obj, const char *key) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    const char *s = cJSON_IsString(v) ? v->valuestring : "";
    if (!strcasecmp(s, "up") || !strcasecmp(s, "green") || !strcasecmp(s, "good")
        || !strcasecmp(s, "success")) {
        return DASH_TONE_UP;
    }
    if (!strcasecmp(s, "down") || !strcasecmp(s, "red") || !strcasecmp(s, "bad")
        || !strcasecmp(s, "alert")) {
        return DASH_TONE_DOWN;
    }
    if (!strcasecmp(s, "accent") || !strcasecmp(s, "amber")
        || !strcasecmp(s, "warning")) {
        return DASH_TONE_ACCENT;
    }
    if (!strcasecmp(s, "blue") || !strcasecmp(s, "info")) return DASH_TONE_BLUE;
    if (!strcasecmp(s, "dim") || !strcasecmp(s, "muted")) return DASH_TONE_DIM;
    return DASH_TONE_DEFAULT;
}

static int64_t expiry(const cJSON *obj, int dflt_s) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, "ttl_s");
    int ttl = cJSON_IsNumber(v) ? (int)v->valuedouble : dflt_s;
    if (ttl <= 0) return 0;
    if (ttl > MAX_TTL_S) ttl = MAX_TTL_S;
    return esp_timer_get_time() + (int64_t)ttl * 1000000;
}

static void parse_row(const cJSON *it, dash_card_row_t *r) {
    memset(r, 0, sizeof(*r));
    str(r->label, sizeof(r->label), it, "label");
    str(r->value, sizeof(r->value), it, "value");
    str(r->detail, sizeof(r->detail), it, "detail");
    r->tone = tone(it, "tone");
    const cJSON *p = cJSON_GetObjectItemCaseSensitive(it, "progress");
    r->progress = cJSON_IsNumber(p)
        ? (int8_t)(p->valuedouble < 0 ? 0 : p->valuedouble > 100 ? 100 : p->valuedouble)
        : -1;
    // Sparkline: any numbers, scaled to their own range and downsampled.
    const cJSON *sp = cJSON_GetObjectItemCaseSensitive(it, "spark");
    int n = cJSON_IsArray(sp) ? cJSON_GetArraySize(sp) : 0;
    if (n >= 2) {
        int m = n < DASH_CARD_SPARK ? n : DASH_CARD_SPARK;
        double lo = 1e300, hi = -1e300, v[DASH_CARD_SPARK];
        for (int k = 0; k < m; k++) {
            const cJSON *e = cJSON_GetArrayItem(sp, (int)((int64_t)k * (n - 1) / (m - 1)));
            v[k] = cJSON_IsNumber(e) ? e->valuedouble : 0;
            if (v[k] < lo) lo = v[k];
            if (v[k] > hi) hi = v[k];
        }
        for (int k = 0; k < m; k++) {
            r->spark[k] = (uint8_t)(hi > lo ? (v[k] - lo) / (hi - lo) * 255 + 0.5 : 128);
        }
        r->spark_n = (uint8_t)m;
    }
}

bool dash_cards_set(const cJSON *c, bool *show, const char **err) {
    *show = false;
    if (!cJSON_IsObject(c)) {
        *err = "card must be a JSON object";
        return false;
    }
    char id[16];
    str(id, sizeof(id), c, "id");
    if (!id[0]) {
        *err = "card needs an id";
        return false;
    }
    if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(c, "remove"))) {
        dash_cards_remove(id);
        return true;
    }
    // Built in a static under the lock: a card is ~1.3 KB, too much for a
    // command handler's stack.
    static dash_card_t card;
    lock();
    memset(&card, 0, sizeof(card));
    memcpy(card.id, id, sizeof(card.id));
    str(card.title, sizeof(card.title), c, "title");
    str(card.sub, sizeof(card.sub), c, "sub");
    str(card.text, sizeof(card.text), c, "text");
    card.tone = tone(c, "tone");
    card.pressed = -1;
    card.expires_us = expiry(c, 0);
    const cJSON *rows = cJSON_GetObjectItemCaseSensitive(c, "rows");
    const cJSON *it = NULL;
    cJSON_ArrayForEach(it, rows) {
        if (card.n_rows >= DASH_CARD_ROWS) break;
        parse_row(it, &card.rows[card.n_rows++]);
    }
    const cJSON *btns = cJSON_GetObjectItemCaseSensitive(c, "buttons");
    cJSON_ArrayForEach(it, btns) {
        if (card.n_buttons >= DASH_CARD_BUTTONS) break;
        dash_card_button_t *b = &card.buttons[card.n_buttons];
        str(b->label, sizeof(b->label), it, "label");
        str(b->id, sizeof(b->id), it, "id");
        str(b->say, sizeof(b->say), it, "say");
        if (!b->label[0]) continue;
        if (!b->id[0]) memcpy(b->id, b->label, sizeof(b->id) - 1);
        card.n_buttons++;
    }
    if (!card.title[0]) memcpy(card.title, card.id, sizeof(card.id));

    int i = 0;
    while (i < s_n_cards && strcmp(s_cards[i].id, id) != 0) i++;
    if (i == s_n_cards) {
        if (s_n_cards == DASH_MAX_CARDS) {
            unlock();
            *err = "4 cards already; remove one first";
            return false;
        }
        s_n_cards++;
    }
    s_cards[i] = card;
    unlock();
    *show = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(c, "show"));
    ESP_LOGI(TAG, "card '%s': %d rows, %d buttons", id, card.n_rows, card.n_buttons);
    return true;
}

bool dash_cards_remove(const char *id) {
    lock();
    int i = 0;
    while (i < s_n_cards && strcmp(s_cards[i].id, id) != 0) i++;
    bool found = i < s_n_cards;
    if (found) {
        memmove(&s_cards[i], &s_cards[i + 1],
                sizeof(s_cards[0]) * (size_t)(s_n_cards - i - 1));
        s_n_cards--;
    }
    unlock();
    return found;
}

int dash_cards_count(void) {
    lock();
    int n = s_n_cards;
    unlock();
    return n;
}

bool dash_cards_get(int index, dash_card_t *out) {
    lock();
    bool ok = index >= 0 && index < s_n_cards;
    if (ok) *out = s_cards[index];
    unlock();
    return ok;
}

int dash_cards_find(const char *id) {
    lock();
    int found = -1;
    for (int i = 0; i < s_n_cards; i++) {
        if (strcmp(s_cards[i].id, id) == 0) found = i;
    }
    unlock();
    return found;
}

void dash_cards_set_pressed(const char *id, int button, const char *status) {
    lock();
    for (int i = 0; i < s_n_cards; i++) {
        if (strcmp(s_cards[i].id, id) != 0) continue;
        s_cards[i].pressed = (int8_t)button;
        snprintf(s_cards[i].status, sizeof(s_cards[i].status), "%s", status);
    }
    unlock();
}

void dash_cards_set_status(const char *id, const char *status) {
    lock();
    for (int i = 0; i < s_n_cards; i++) {
        if (strcmp(s_cards[i].id, id) == 0) {
            snprintf(s_cards[i].status, sizeof(s_cards[i].status), "%s", status);
        }
    }
    unlock();
}

bool dash_banner_push(const cJSON *n, const char **err) {
    if (!cJSON_IsObject(n)) {
        *err = "notification must be a JSON object";
        return false;
    }
    dash_banner_t b = {.active = true};
    str(b.text, sizeof(b.text), n, "text");
    if (!b.text[0]) {
        *err = "text is required";
        return false;
    }
    str(b.detail, sizeof(b.detail), n, "detail");
    str(b.card, sizeof(b.card), n, "card");
    b.tone = tone(n, "level");
    if (b.tone == DASH_TONE_DEFAULT) b.tone = DASH_TONE_BLUE;
    b.expires_us = expiry(n, BANNER_TTL_S);
    lock();
    if (s_n_banners == BANNER_QUEUE) {
        // Full: drop the oldest waiting one (keep what is on screen).
        memmove(&s_banners[1], &s_banners[2],
                sizeof(s_banners[0]) * (BANNER_QUEUE - 2));
        s_n_banners--;
    }
    s_banners[s_n_banners++] = b;
    unlock();
    return true;
}

static void pop_banner_locked(void) {
    if (!s_n_banners) return;
    memmove(&s_banners[0], &s_banners[1],
            sizeof(s_banners[0]) * (size_t)(s_n_banners - 1));
    s_n_banners--;
    // The next one's clock starts when it appears.
    if (s_n_banners && s_banners[0].expires_us) {
        s_banners[0].expires_us = esp_timer_get_time() + BANNER_TTL_S * 1000000LL;
    }
}

bool dash_banner_current(dash_banner_t *out) {
    lock();
    bool ok = s_n_banners > 0;
    if (ok) *out = s_banners[0];
    unlock();
    return ok;
}

void dash_banner_dismiss(void) {
    lock();
    pop_banner_locked();
    unlock();
}

bool dash_cards_tick(void) {
    int64_t now = esp_timer_get_time();
    bool changed = false;
    lock();
    for (int i = s_n_cards - 1; i >= 0; i--) {
        if (s_cards[i].expires_us && now >= s_cards[i].expires_us) {
            memmove(&s_cards[i], &s_cards[i + 1],
                    sizeof(s_cards[0]) * (size_t)(s_n_cards - i - 1));
            s_n_cards--;
            changed = true;
        }
    }
    if (s_n_banners && s_banners[0].expires_us && now >= s_banners[0].expires_us) {
        pop_banner_locked();
        changed = true;
    }
    unlock();
    return changed;
}
