/*
 * Taps on the dashboard that Muse should know about (card buttons).
 *
 * Each event is sent to Muse as a chat message over the board's existing
 * Link session (POST /chat/stream on the VM, CONFIG_HOMEHUB_DASHBOARD_MUSE_
 * CHAT), the same way Muse's own boards send text turns; the board only
 * sends and never subscribes to replies, so Muse answers in the app and/or
 * by updating the board. Every event is also queued here, so Muse can poll
 * them with dashboard.events when the chat path is unavailable.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

// A card button was tapped. `say` is the button's own message for Muse ("" to
// describe the tap). Returns the status text to show on the card.
const char *dash_events_button(const char *card_id, const char *card_title,
                               const char *button_id, const char *label,
                               const char *say);

// dashboard.events: the queued events as a JSON array (oldest first); with
// `clear`, they are removed.
cJSON *dash_events_take(bool clear);

// One-line status for dashboard.debug.
void dash_events_status(char *buf, size_t n);

#ifdef __cplusplus
}
#endif
