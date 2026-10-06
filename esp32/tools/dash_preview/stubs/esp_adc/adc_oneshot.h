#pragma once
#include "esp_err.h"
typedef void *adc_oneshot_unit_handle_t;
typedef enum { ADC_UNIT_1 } adc_unit_t;
typedef enum { ADC_CHANNEL_6 = 6 } adc_channel_t;
typedef enum { ADC_ATTEN_DB_0 } adc_atten_t;
typedef enum { ADC_BITWIDTH_DEFAULT } adc_bitwidth_t;
typedef struct { adc_unit_t unit_id; } adc_oneshot_unit_init_cfg_t;
typedef struct { adc_atten_t atten; adc_bitwidth_t bitwidth; } adc_oneshot_chan_cfg_t;
esp_err_t adc_oneshot_new_unit(const adc_oneshot_unit_init_cfg_t *c, adc_oneshot_unit_handle_t *h);
esp_err_t adc_oneshot_config_channel(adc_oneshot_unit_handle_t h, adc_channel_t ch, const adc_oneshot_chan_cfg_t *c);
esp_err_t adc_oneshot_read(adc_oneshot_unit_handle_t h, adc_channel_t ch, int *out);
