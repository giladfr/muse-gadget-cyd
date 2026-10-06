#pragma once
#include <sys/time.h>
#include "esp_err.h"
typedef void (*esp_sntp_time_cb_t)(struct timeval *tv);
typedef struct { const char *server; esp_sntp_time_cb_t sync_cb; } esp_sntp_config_t;
#define ESP_NETIF_SNTP_DEFAULT_CONFIG(s) { .server = (s) }
esp_err_t esp_netif_sntp_init(const esp_sntp_config_t *c);
