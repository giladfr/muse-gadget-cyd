#pragma once
#include <stdint.h>
#include "esp_err.h"
typedef int gpio_num_t;
typedef enum {GPIO_MODE_INPUT=1, GPIO_MODE_OUTPUT=2} gpio_mode_t;
typedef enum {GPIO_INTR_NEGEDGE=2} gpio_int_type_t;
typedef struct { uint64_t pin_bit_mask; gpio_mode_t mode; int pull_up_en; int pull_down_en; gpio_int_type_t intr_type; } gpio_config_t;
typedef void (*gpio_isr_t)(void *);
esp_err_t gpio_config(const gpio_config_t *c);
esp_err_t gpio_set_level(gpio_num_t p, uint32_t l);
int gpio_get_level(gpio_num_t p);
esp_err_t gpio_install_isr_service(int f);
esp_err_t gpio_set_intr_type(gpio_num_t p, gpio_int_type_t t);
esp_err_t gpio_isr_handler_add(gpio_num_t p, gpio_isr_t h, void *a);
esp_err_t gpio_intr_enable(gpio_num_t p);
esp_err_t gpio_intr_disable(gpio_num_t p);
