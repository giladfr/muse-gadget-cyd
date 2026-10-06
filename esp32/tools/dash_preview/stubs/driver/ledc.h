#pragma once
#include <stdint.h>
#include "esp_err.h"
typedef enum { LEDC_LOW_SPEED_MODE } ledc_mode_t;
typedef enum { LEDC_TIMER_1 = 1 } ledc_timer_t;
typedef enum { LEDC_CHANNEL_4 = 4 } ledc_channel_t;
typedef enum { LEDC_TIMER_10_BIT = 10 } ledc_timer_bit_t;
typedef enum { LEDC_AUTO_CLK } ledc_clk_cfg_t;
typedef enum { LEDC_FADE_NO_WAIT } ledc_fade_mode_t;
typedef struct { ledc_mode_t speed_mode; ledc_timer_bit_t duty_resolution; ledc_timer_t timer_num; uint32_t freq_hz; ledc_clk_cfg_t clk_cfg; } ledc_timer_config_t;
typedef struct { int gpio_num; ledc_mode_t speed_mode; ledc_channel_t channel; ledc_timer_t timer_sel; uint32_t duty; int hpoint; } ledc_channel_config_t;
esp_err_t ledc_timer_config(const ledc_timer_config_t *c);
esp_err_t ledc_channel_config(const ledc_channel_config_t *c);
esp_err_t ledc_fade_func_install(int f);
esp_err_t ledc_set_fade_time_and_start(ledc_mode_t m, ledc_channel_t c, uint32_t d, uint32_t ms, ledc_fade_mode_t w);
esp_err_t ledc_set_duty(ledc_mode_t m, ledc_channel_t c, uint32_t d);
esp_err_t ledc_update_duty(ledc_mode_t m, ledc_channel_t c);
