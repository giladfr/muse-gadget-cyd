/*
 * Dashboard network provider: polls the bridge's plain-HTTP JSON endpoint
 * for stocks/weather. Plain HTTP only: this board has no PSRAM for TLS.
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Start the poll task. Safe to call once.
void dash_net_start(void);
// Force an immediate poll (e.g. after takeover ends).
void dash_net_poll_now(void);

#ifdef __cplusplus
}
#endif
