/*
 * Dashboard backlight: PWM on the CYD's backlight pin (GPIO 21), following
 * the ambient light sensor (GPIO 34), a night schedule and an idle timeout.
 * Changes fade instead of jumping.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Take over the backlight pin. Call from the dashboard task once active.
void dash_backlight_init(void);

// Re-evaluate the target level (ambient light, schedule, idle) and fade to
// it. Cheap; call on every dashboard task wake-up.
void dash_backlight_update(void);

// A touch (or a pushed image) happened. Returns true if the screen was dim
// enough that this touch should only wake it, not act.
bool dash_backlight_activity(void);

// One-line diagnostics: LDR reading, target and current level.
void dash_backlight_debug(char *buf, size_t n);

#ifdef __cplusplus
}
#endif
