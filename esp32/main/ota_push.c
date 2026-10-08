/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "ota_push.h"

#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "ota.h"
#include "psa/crypto.h"

static const char *TAG = "link.ota_push";

#define IDLE_TIMEOUT_US (120 * 1000000LL)
#define REBOOT_DELAY_US (3 * 1000000LL)

// Owned under s_lock: the session task writes, the idle timer abandons.
static SemaphoreHandle_t s_lock;
static bool s_active;
static esp_ota_handle_t s_handle;
static const esp_partition_t *s_part;
static uint32_t s_size, s_next;
static uint8_t s_want[32];
static psa_hash_operation_t s_hash;
static int s_last_pct = -1;
static esp_timer_handle_t s_idle_timer, s_reboot_timer;
static esp_app_desc_t s_desc;

static void lock(void) {
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void unlock(void) {
    xSemaphoreGive(s_lock);
}

static void report(int pct) {
    if (pct == s_last_pct) return;
    s_last_pct = pct;
    ota_report_progress(pct);
}

// Caller holds the lock.
static void stop_locked(bool failed) {
    if (!s_active) return;
    esp_ota_abort(s_handle);
    psa_hash_abort(&s_hash);
    s_active = false;
    if (s_idle_timer) esp_timer_stop(s_idle_timer);
    if (failed) report(-1);
    s_last_pct = -1;
}

static void idle_expired(void *arg) {
    (void)arg;
    lock();
    if (s_active) ESP_LOGW(TAG, "no chunk for 2 minutes at %u/%u; abandoning",
                           (unsigned)s_next, (unsigned)s_size);
    stop_locked(true);
    unlock();
}

static void reboot_now(void *arg) {
    (void)arg;
    esp_restart();
}

static void touch_idle(void) {
    if (!s_idle_timer) {
        const esp_timer_create_args_t a = {.callback = idle_expired, .name = "ota_push"};
        if (esp_timer_create(&a, &s_idle_timer) != ESP_OK) return;
    }
    esp_timer_stop(s_idle_timer);
    esp_timer_start_once(s_idle_timer, IDLE_TIMEOUT_US);
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool parse_sha(const char *hex, uint8_t out[32]) {
    if (!hex || strlen(hex) != 64) return false;
    for (int i = 0; i < 32; i++) {
        int a = hexval(hex[2 * i]), b = hexval(hex[2 * i + 1]);
        if (a < 0 || b < 0) return false;
        out[i] = (uint8_t)(a << 4 | b);
    }
    return true;
}

static int b64val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

// Decode base64 in place (output never overtakes input). Whitespace is
// skipped; returns the decoded length, or -1 if it isn't base64.
static int b64_decode_in_place(char *s) {
    uint8_t *out = (uint8_t *)s;
    int n = 0, bits = 0, pad = 0;
    uint32_t acc = 0;
    for (const char *p = s; *p; p++) {
        if (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') continue;
        if (*p == '=') {
            pad++;
            continue;
        }
        int v = b64val(*p);
        if (v < 0 || pad) return -1;
        acc = acc << 6 | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[n++] = (uint8_t)(acc >> bits);
        }
    }
    return pad > 2 ? -1 : n;
}

bool ota_push_begin(uint32_t size, const char *sha256_hex, uint32_t *next,
                    const char **err) {
    uint8_t want[32];
    if (!parse_sha(sha256_hex, want)) {
        *err = "sha256 must be 64 hex characters";
        return false;
    }
    lock();
    if (s_active && s_size == size && memcmp(s_want, want, 32) == 0) {
        // The same image: carry on where it got to.
        *next = s_next;
        touch_idle();
        unlock();
        ESP_LOGI(TAG, "resuming at %u/%u", (unsigned)s_next, (unsigned)size);
        return true;
    }
    stop_locked(false);
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        unlock();
        *err = "no OTA slot to write";
        return false;
    }
    if (size < 1024 || size > part->size) {
        unlock();
        *err = "size doesn't fit the OTA slot";
        return false;
    }
    if (psa_crypto_init() != PSA_SUCCESS) {
        unlock();
        *err = "crypto unavailable";
        return false;
    }
    s_hash = (psa_hash_operation_t)PSA_HASH_OPERATION_INIT;
    if (psa_hash_setup(&s_hash, PSA_ALG_SHA_256) != PSA_SUCCESS) {
        unlock();
        *err = "crypto unavailable";
        return false;
    }
    // Sequential writes erase each sector just before it is written, so
    // begin returns at once instead of erasing ~2 MB first.
    esp_err_t e = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &s_handle);
    if (e != ESP_OK) {
        psa_hash_abort(&s_hash);
        unlock();
        *err = "could not open the OTA slot";
        return false;
    }
    s_active = true;
    s_part = part;
    s_size = size;
    s_next = 0;
    memcpy(s_want, want, 32);
    memset(&s_desc, 0, sizeof(s_desc));
    s_last_pct = -1;
    report(0);
    touch_idle();
    *next = 0;
    unlock();
    ESP_LOGI(TAG, "receiving %u bytes into %s", (unsigned)size, part->label);
    return true;
}

bool ota_push_write(uint32_t offset, char *b64, uint32_t *next, const char **err) {
    lock();
    *next = s_next;
    if (!s_active) {
        unlock();
        *err = "no update in progress (ota.begin first)";
        return false;
    }
    if (offset != s_next) {
        unlock();
        *err = "out of order: write at `next`";
        return false;
    }
    int n = b64 ? b64_decode_in_place(b64) : -1;
    if (n <= 0 || n > OTA_PUSH_CHUNK_MAX || s_next + (uint32_t)n > s_size) {
        unlock();
        *err = n > OTA_PUSH_CHUNK_MAX ? "chunk too large"
             : n > 0 ? "chunk runs past size" : "data must be base64";
        return false;
    }
    if (esp_ota_write(s_handle, b64, (size_t)n) != ESP_OK) {
        ESP_LOGE(TAG, "flash write failed at %u", (unsigned)s_next);
        stop_locked(true);
        unlock();
        *err = "flash write failed; start again";
        return false;
    }
    psa_hash_update(&s_hash, (const uint8_t *)b64, (size_t)n);
    // The image's app description sits right after the image and first
    // segment headers.
    uint32_t d0 = 24 + 8;
    if (s_next < d0 + sizeof(s_desc) && s_next + (uint32_t)n > d0) {
        for (uint32_t i = 0; i < (uint32_t)n; i++) {
            uint32_t at = s_next + i;
            if (at >= d0 && at < d0 + sizeof(s_desc)) {
                ((uint8_t *)&s_desc)[at - d0] = (uint8_t)b64[i];
            }
        }
    }
    s_next += (uint32_t)n;
    *next = s_next;
    report((int)((uint64_t)s_next * 100 / s_size));
    touch_idle();
    unlock();
    return true;
}

bool ota_push_finish(const char **version, const char **err) {
    static char ver[sizeof(s_desc.version) + 1];
    lock();
    if (!s_active) {
        unlock();
        *err = "no update in progress";
        return false;
    }
    if (s_next != s_size) {
        unlock();
        *err = "not all of the image has arrived (see next)";
        return false;
    }
    uint8_t got[32];
    size_t len = 0;
    bool hashed = psa_hash_finish(&s_hash, got, sizeof(got), &len) == PSA_SUCCESS
                  && len == 32;
    if (!hashed || memcmp(got, s_want, 32) != 0) {
        ESP_LOGE(TAG, "SHA-256 mismatch; discarding the update");
        esp_ota_abort(s_handle);
        s_active = false;
        if (s_idle_timer) esp_timer_stop(s_idle_timer);
        report(-1);
        unlock();
        *err = "sha256 doesn't match what arrived; start again";
        return false;
    }
    s_active = false;
    if (s_idle_timer) esp_timer_stop(s_idle_timer);
    // Verifies the image, and its signature on signed builds.
    esp_err_t e = esp_ota_end(s_handle);
    if (e == ESP_OK) e = esp_ota_set_boot_partition(s_part);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "image rejected: %s", esp_err_to_name(e));
        report(-1);
        unlock();
        *err = e == ESP_ERR_OTA_VALIDATE_FAILED
                   ? "image failed verification (not a signed image for this board?)"
                   : "could not switch to the new firmware";
        return false;
    }
    snprintf(ver, sizeof(ver), "%.*s", (int)sizeof(s_desc.version), s_desc.version);
    *version = ver;
    ESP_LOGI(TAG, "update %s verified; rebooting into %s", ver, s_part->label);
    report(100);
    // Late enough for the reply to reach Muse first.
    const esp_timer_create_args_t a = {.callback = reboot_now, .name = "ota_reboot"};
    if (!s_reboot_timer && esp_timer_create(&a, &s_reboot_timer) != ESP_OK) {
        unlock();
        esp_restart();
    }
    esp_timer_start_once(s_reboot_timer, REBOOT_DELAY_US);
    unlock();
    return true;
}

void ota_push_abort(void) {
    lock();
    if (s_active) ESP_LOGI(TAG, "update abandoned at %u/%u", (unsigned)s_next,
                           (unsigned)s_size);
    stop_locked(true);
    unlock();
}

bool ota_push_status(uint32_t *size, uint32_t *next) {
    lock();
    bool active = s_active;
    *size = s_size;
    *next = s_next;
    unlock();
    return active;
}
