#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
typedef void *esp_http_client_handle_t;
typedef enum { HTTP_EVENT_ERROR, HTTP_EVENT_ON_DATA } esp_http_client_event_id_t;
typedef struct { esp_http_client_event_id_t event_id; void *data; int data_len; void *user_data; } esp_http_client_event_t;
typedef esp_err_t (*http_event_handle_cb)(esp_http_client_event_t *e);
typedef struct {
    const char *url; int timeout_ms; int buffer_size; int buffer_size_tx;
    bool disable_auto_redirect; bool keep_alive_enable; int max_redirection_count;
    esp_err_t (*crt_bundle_attach)(void *); http_event_handle_cb event_handler; void *user_data; const char *user_agent;
} esp_http_client_config_t;
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *c);
esp_err_t esp_http_client_set_url(esp_http_client_handle_t c, const char *url);
esp_err_t esp_http_client_perform(esp_http_client_handle_t c);
esp_err_t esp_http_client_set_header(esp_http_client_handle_t c, const char *k, const char *v);
esp_err_t esp_http_client_open(esp_http_client_handle_t c, int len);
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t c);
int esp_http_client_get_status_code(esp_http_client_handle_t c);
int esp_http_client_read(esp_http_client_handle_t c, char *b, int n);
esp_err_t esp_http_client_close(esp_http_client_handle_t c);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t c);
