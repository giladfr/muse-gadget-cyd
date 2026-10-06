#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
typedef void *esp_http_client_handle_t;
typedef struct { const char *url; int timeout_ms; int buffer_size; bool disable_auto_redirect; } esp_http_client_config_t;
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *c);
esp_err_t esp_http_client_open(esp_http_client_handle_t c, int len);
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t c);
int esp_http_client_get_status_code(esp_http_client_handle_t c);
int esp_http_client_read(esp_http_client_handle_t c, char *b, int n);
esp_err_t esp_http_client_close(esp_http_client_handle_t c);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t c);
