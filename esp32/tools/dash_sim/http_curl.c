/*
 * esp_http_client on top of libcurl, so the firmware's HTTP code (live
 * quotes, the bridge poller) runs against the real network on a computer.
 * Covers the calls the dashboard uses: perform with an event handler, and
 * open / fetch_headers / read / close (served from a buffered response).
 */
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_http_client.h"

typedef struct {
    esp_http_client_config_t cfg;
    char url[512];
    struct curl_slist *headers;
    int status;
    // Buffered body for open/read.
    char *body;
    size_t len, pos;
} client_t;

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *c) {
    static int once;
    if (!once++) curl_global_init(CURL_GLOBAL_DEFAULT);
    client_t *h = calloc(1, sizeof(*h));
    if (!h) return NULL;
    h->cfg = *c;
    snprintf(h->url, sizeof(h->url), "%s", c->url ? c->url : "");
    return h;
}

esp_err_t esp_http_client_set_url(esp_http_client_handle_t c, const char *url) {
    client_t *h = c;
    snprintf(h->url, sizeof(h->url), "%s", url);
    return ESP_OK;
}

esp_err_t esp_http_client_set_header(esp_http_client_handle_t c, const char *k, const char *v) {
    client_t *h = c;
    char line[512];
    snprintf(line, sizeof(line), "%s: %s", k, v);
    h->headers = curl_slist_append(h->headers, line);
    return ESP_OK;
}

typedef struct {
    client_t *h;
    bool to_handler;
} sink_t;

static size_t on_body(char *p, size_t sz, size_t n, void *ud) {
    sink_t *s = ud;
    size_t len = sz * n;
    if (s->to_handler && s->h->cfg.event_handler) {
        esp_http_client_event_t e = {
            .event_id = HTTP_EVENT_ON_DATA, .data = p, .data_len = (int)len,
            .user_data = s->h->cfg.user_data,
        };
        s->h->cfg.event_handler(&e);
    } else {
        char *b = realloc(s->h->body, s->h->len + len + 1);
        if (!b) return 0;
        memcpy(b + s->h->len, p, len);
        s->h->body = b;
        s->h->len += len;
    }
    return len;
}

static esp_err_t run(client_t *h, bool to_handler) {
    CURL *curl = curl_easy_init();
    if (!curl) return ESP_FAIL;
    sink_t s = {h, to_handler};
    curl_easy_setopt(curl, CURLOPT_URL, h->url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &s);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)(h->cfg.timeout_ms ? h->cfg.timeout_ms : 10000));
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, h->cfg.disable_auto_redirect ? 0L : 1L);
    if (h->cfg.user_agent) curl_easy_setopt(curl, CURLOPT_USERAGENT, h->cfg.user_agent);
    if (h->headers) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, h->headers);
    CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    h->status = (int)status;
    if (rc != CURLE_OK) {
        fprintf(stderr, "[http] %s: %s\n", h->url, curl_easy_strerror(rc));
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t esp_http_client_perform(esp_http_client_handle_t c) {
    return run(c, true);
}

esp_err_t esp_http_client_open(esp_http_client_handle_t c, int len) {
    (void)len;
    client_t *h = c;
    free(h->body);
    h->body = NULL;
    h->len = h->pos = 0;
    return run(h, false);
}

int64_t esp_http_client_fetch_headers(esp_http_client_handle_t c) {
    return (int64_t)((client_t *)c)->len;
}

int esp_http_client_get_status_code(esp_http_client_handle_t c) {
    return ((client_t *)c)->status;
}

int esp_http_client_read(esp_http_client_handle_t c, char *b, int n) {
    client_t *h = c;
    size_t left = h->len - h->pos;
    size_t take = left < (size_t)n ? left : (size_t)n;
    memcpy(b, h->body + h->pos, take);
    h->pos += take;
    return (int)take;
}

esp_err_t esp_http_client_close(esp_http_client_handle_t c) {
    (void)c;
    return ESP_OK;
}

esp_err_t esp_http_client_cleanup(esp_http_client_handle_t c) {
    client_t *h = c;
    curl_slist_free_all(h->headers);
    free(h->body);
    free(h);
    return ESP_OK;
}
