#pragma once
#include <stdbool.h>
#include <stdint.h>
void dashboard_display_set_active(bool on);
bool dashboard_display_draw(int x, int y, int w, int h, const uint16_t *pixels);
bool dashboard_display_draw_start(int x, int y, int w, int h, const uint16_t *pixels);
void dashboard_display_draw_wait(void);
