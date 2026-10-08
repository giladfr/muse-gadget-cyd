// sd_fetch.c — download a file from a URL onto the SD card.

#include "sd_fetch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "psa/crypto.h"
#include "sd_card.h"

static const char *TAG = "link.sd_fetch";

#define MAX_BYTES (8 * 1024 * 1024)
#define CHUNK 1024
#define HTTP_STACK 4096
// TLS handshakes need far more stack (and ~40 KB of heap).
#define HTTPS_STACK 8192

typedef struct {
    char url[256];
    char path[112];      // /sdcard/<path>
    char sha256[65];     // lowercase hex, or empty
    sd_fetch_done_cb cb;
    void *user;
} job_t;

static volatile bool s_busy;

// mkdir -p for the directories in `path` (not the file itself).
static void make_parents(const char *path) {
    char tmp[sizeof(((job_t *)0)->path)];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + strlen("/sdcard/"); *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        mkdir(tmp, 0775);
        *p = '/';
    }
}

static const char *download(job_t *j, size_t *bytes) {
    esp_http_client_config_t cfg = {
        .url = j->url,
        .timeout_ms = 15000,
        .buffer_size = CHUNK,
        .max_redirection_count = 3,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return "http init failed";
    char part[sizeof(j->path) + 8];
    snprintf(part, sizeof(part), "%s.part", j->path);
    const char *err = NULL;
    FILE *f = NULL;
    uint8_t *buf = malloc(CHUNK);
    psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
    bool hashing = j->sha256[0] && psa_crypto_init() == PSA_SUCCESS
                   && psa_hash_setup(&op, PSA_ALG_SHA_256) == PSA_SUCCESS;
    if (j->sha256[0] && !hashing) err = "sha256 unavailable";

    if (!err && !buf) err = "out of memory";
    if (!err && esp_http_client_open(c, 0) != ESP_OK) err = "connect failed";
    if (!err) {
        int64_t len = esp_http_client_fetch_headers(c);
        int status = esp_http_client_get_status_code(c);
        if (status != 200) {
            err = "HTTP status not 200";
        } else if (len > MAX_BYTES) {
            err = "file too large";
        }
    }
    if (!err) {
        make_parents(j->path);
        f = fopen(part, "wb");
        if (!f) err = "cannot create file";
    }
    while (!err) {
        int n = esp_http_client_read(c, (char *)buf, CHUNK);
        if (n < 0) {
            err = "download failed";
            break;
        }
        if (n == 0) {
            if (!esp_http_client_is_complete_data_received(c)) {
                err = "download incomplete";
            }
            break;
        }
        if (*bytes + (size_t)n > MAX_BYTES) {
            err = "file too large";
        } else if (fwrite(buf, 1, (size_t)n, f) != (size_t)n) {
            err = "SD write failed (card full?)";
        } else {
            if (hashing) psa_hash_update(&op, buf, (size_t)n);
            *bytes += (size_t)n;
        }
    }
    if (f && fclose(f) != 0 && !err) err = "SD write failed";
    if (!err && hashing) {
        uint8_t digest[32];
        size_t dlen = 0;
        char hex[65];
        if (psa_hash_finish(&op, digest, sizeof(digest), &dlen) != PSA_SUCCESS) {
            err = "sha256 failed";
        } else {
            for (int i = 0; i < 32; i++) sprintf(hex + 2 * i, "%02x", digest[i]);
            if (strcmp(hex, j->sha256) != 0) err = "sha256 mismatch";
        }
    } else if (hashing) {
        psa_hash_abort(&op);
    }
    if (!err) {
        remove(j->path);  // FAT rename does not replace
        if (rename(part, j->path) != 0) err = "rename failed";
    }
    if (err && f) remove(part);
    free(buf);
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return err;
}

static void fetch_task(void *arg) {
    job_t *j = arg;
    size_t bytes = 0;
    const char *err = download(j, &bytes);
    if (err) {
        ESP_LOGW(TAG, "%s: %s", j->path, err);
    } else {
        ESP_LOGI(TAG, "%s: %u bytes", j->path, (unsigned)bytes);
    }
    if (j->cb) j->cb(err, bytes, j->user);
    free(j);
    s_busy = false;
    vTaskDelete(NULL);
}

static bool valid_path(const char *p) {
    size_t n = strlen(p);
    return n > 0 && n < 96 && p[0] != '/' && !strstr(p, "..")
           && p[n - 1] != '/';
}

static bool valid_sha(const char *s, char out[65]) {
    if (strlen(s) != 64) return false;
    for (int i = 0; i < 64; i++) {
        char c = s[i];
        if (c >= 'A' && c <= 'F') c = (char)(c - 'A' + 'a');
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
        out[i] = c;
    }
    out[64] = '\0';
    return true;
}

bool sd_fetch_start(const char *url, const char *path, const char *sha256_hex,
                    sd_fetch_done_cb cb, void *user, const char **err) {
    bool https = url && strncmp(url, "https://", 8) == 0;
    if (!url || (!https && strncmp(url, "http://", 7) != 0)
        || strlen(url) >= sizeof(((job_t *)0)->url)) {
        *err = "url must be http(s):// and under 256 chars";
        return false;
    }
    if (!path || !valid_path(path)) {
        *err = "path must be relative, without '..'";
        return false;
    }
    if (!sd_card_mounted() && !sd_card_init()) {
        *err = "no SD card";
        return false;
    }
    if (s_busy) {
        *err = "another sd.fetch is running";
        return false;
    }
    job_t *j = calloc(1, sizeof(*j));
    if (!j) {
        *err = "out of memory";
        return false;
    }
    if (sha256_hex && sha256_hex[0] && !valid_sha(sha256_hex, j->sha256)) {
        free(j);
        *err = "sha256 must be 64 hex characters";
        return false;
    }
    snprintf(j->url, sizeof(j->url), "%s", url);
    snprintf(j->path, sizeof(j->path), "/sdcard/%s", path);
    j->cb = cb;
    j->user = user;
    s_busy = true;
    if (xTaskCreate(fetch_task, "sd_fetch", https ? HTTPS_STACK : HTTP_STACK, j,
                    3, NULL) != pdPASS) {
        s_busy = false;
        free(j);
        *err = "could not start download task";
        return false;
    }
    return true;
}
