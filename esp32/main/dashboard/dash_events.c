/*
 * Dashboard events implementation.
 */
#include "dash_events.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "dash_cards.h"
#include "dashboard.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"

#if CONFIG_HOMEHUB_DASHBOARD_MUSE_CHAT
#include "noise_control.h"
#endif

static const char *TAG = "dash.events";

#define QUEUE 8

typedef struct {
    int64_t at;          // unix seconds (0 if the clock isn't set)
    char card[16];
    char button[16];
    char label[16];
    char message[160];   // what was (or would have been) sent to Muse
    bool delivered;      // Muse acknowledged the chat message
} event_t;

static event_t s_q[QUEUE];
static int s_n;
static unsigned s_sent, s_acked, s_failed;
static int s_last_status;
static SemaphoreHandle_t s_lock;

static void lock(void) {
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void unlock(void) {
    xSemaphoreGive(s_lock);
}

#if CONFIG_HOMEHUB_DASHBOARD_MUSE_CHAT

// JSON-escape `in` into out (quotes, backslashes, control characters).
static void json_escape(char *out, size_t n, const char *in) {
    size_t o = 0;
    for (; *in && o + 7 < n; in++) {
        unsigned char c = (unsigned char)*in;
        if (c == '"' || c == '\\') {
            out[o++] = '\\';
            out[o++] = (char)c;
        } else if (c < 0x20) {
            o += (size_t)snprintf(out + o, n - o, "\\u%04x", c);
        } else {
            out[o++] = (char)c;
        }
    }
    out[o] = '\0';
}

// The ack arrives on the session task: record it and update the card.
typedef struct {
    char card[16];
    int slot;   // queue index at send time (best effort; the queue may move)
} ack_ctx_t;

static void on_ack(void *ctx, int status, const uint8_t *data, size_t len,
                   bool end) {
    (void)data;
    (void)len;
    ack_ctx_t *a = ctx;
    if (status > 0) {
        s_last_status = status;
        bool ok = status >= 200 && status < 300;
        if (ok) s_acked++; else s_failed++;
        dash_cards_set_status(a->card, ok ? "Sent to Muse" : "Muse didn't take it");
        if (ok) {
            lock();
            if (a->slot >= 0 && a->slot < s_n) s_q[a->slot].delivered = true;
            unlock();
        }
        dashboard_data_updated();
    } else if (status < 0) {
        s_failed++;
        dash_cards_set_status(a->card, "Queued for Muse");
        dashboard_data_updated();
    }
    if (end || status < 0) free(a);
}

// POST /chat/stream {"message": ..., "output_modality": "text"}: a text turn,
// as muse_chat_link.c sends them. False if the session can't take it now.
static bool send_chat(const char *message, const char *card, int slot) {
    if (!noise_ctrl_is_connected()) return false;
    char *body = malloc(400);
    ack_ctx_t *a = calloc(1, sizeof(*a));
    if (!body || !a) {
        free(body);
        free(a);
        return false;
    }
    char esc[320];
    json_escape(esc, sizeof(esc), message);
    int len = snprintf(body, 400, "{\"message\":\"%s\",\"output_modality\":\"text\"}",
                       esc);
    snprintf(a->card, sizeof(a->card), "%s", card);
    a->slot = slot;
    char req_id[40];
    snprintf(req_id, sizeof(req_id), "cyd-%08lx-%08lx",
             (unsigned long)esp_random(), (unsigned long)esp_random());
    const char *headers[] = {"x-request-id", req_id, "x-app-id", "hatch-web",
                             "Content-Type", "application/json",
                             "Accept", "application/json", NULL};
    int64_t id = noise_ctrl_req_open("POST", "/chat/stream", headers, false,
                                     on_ack, a);
    bool ok = id && noise_ctrl_req_send(id, body, (size_t)len, true, 200);
    free(body);
    if (!id) free(a);  // never opened: no callback will come
    else if (!ok) noise_ctrl_req_cancel(id);  // the callback ends it (and frees a)
    if (ok) s_sent++;
    return ok;
}

#endif

const char *dash_events_button(const char *card_id, const char *card_title,
                               const char *button_id, const char *label,
                               const char *say) {
    event_t e = {0};
    e.at = time(NULL) > 1704067200 ? (int64_t)time(NULL) : 0;
    snprintf(e.card, sizeof(e.card), "%s", card_id);
    snprintf(e.button, sizeof(e.button), "%s", button_id);
    snprintf(e.label, sizeof(e.label), "%s", label);
    // Tell Muse what happened in words it can act on: the button's own
    // message, tagged with where it came from.
    if (say && say[0]) {
        snprintf(e.message, sizeof(e.message), "[Desk display] %s", say);
    } else {
        snprintf(e.message, sizeof(e.message),
                 "[Desk display] Tapped \"%s\" on \"%s\" (card %s, button %s)",
                 label, card_title, card_id, button_id);
    }
    lock();
    if (s_n == QUEUE) {
        memmove(&s_q[0], &s_q[1], sizeof(s_q[0]) * (QUEUE - 1));
        s_n--;
    }
    int slot = s_n;
    s_q[s_n++] = e;
    unlock();
    ESP_LOGI(TAG, "%s", e.message);
#if CONFIG_HOMEHUB_DASHBOARD_MUSE_CHAT
    if (send_chat(e.message, card_id, slot)) return "Sending...";
#else
    (void)slot;
#endif
    return "Queued for Muse";
}

cJSON *dash_events_take(bool clear) {
    cJSON *arr = cJSON_CreateArray();
    lock();
    for (int i = 0; arr && i < s_n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "at", (double)s_q[i].at);
        cJSON_AddStringToObject(o, "card", s_q[i].card);
        cJSON_AddStringToObject(o, "button", s_q[i].button);
        cJSON_AddStringToObject(o, "label", s_q[i].label);
        cJSON_AddStringToObject(o, "message", s_q[i].message);
        cJSON_AddBoolToObject(o, "delivered", s_q[i].delivered);
        cJSON_AddItemToArray(arr, o);
    }
    if (clear) s_n = 0;
    unlock();
    return arr;
}

void dash_events_status(char *buf, size_t n) {
#if CONFIG_HOMEHUB_DASHBOARD_MUSE_CHAT
    const char *mode = "";
#else
    const char *mode = " (chat off)";
#endif
    snprintf(buf, n, "events queued=%d sent=%u acked=%u failed=%u http=%d%s", s_n,
             s_sent, s_acked, s_failed, s_last_status, mode);
    (void)TAG;
}
