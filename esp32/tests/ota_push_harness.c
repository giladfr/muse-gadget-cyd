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

// Drives main/ota_push.c the way Muse would push an image (see
// test_ota_push.py): out-of-order and oversized chunks, a resume, a SHA-256
// mismatch, the idle timeout, and a clean update into a fake OTA slot.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/semphr.h"
#include "ota.h"
#include "ota_push.h"
#include "psa/crypto.h"

#define SLOT_SIZE (64 * 1024)

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

// ---- fakes ------------------------------------------------------------------

static esp_partition_t s_slot = {"ota_0", SLOT_SIZE};
static uint8_t s_flash[SLOT_SIZE];
static size_t s_flash_n;
static int s_open, s_boot_set, s_restarts, s_end_result = ESP_OK, s_last_pct = -2;

const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *s) {
    (void)s;
    return &s_slot;
}
esp_err_t esp_ota_begin(const esp_partition_t *p, size_t size, esp_ota_handle_t *h) {
    (void)p;
    if (size != OTA_WITH_SEQUENTIAL_WRITES) return ESP_FAIL;
    memset(s_flash, 0xff, sizeof(s_flash));
    s_flash_n = 0;
    s_open++;
    *h = 7;
    return ESP_OK;
}
esp_err_t esp_ota_write(esp_ota_handle_t h, const void *d, size_t n) {
    if (h != 7 || !s_open || s_flash_n + n > SLOT_SIZE) return ESP_FAIL;
    memcpy(s_flash + s_flash_n, d, n);
    s_flash_n += n;
    return ESP_OK;
}
esp_err_t esp_ota_end(esp_ota_handle_t h) {
    (void)h;
    s_open--;
    return s_end_result;
}
esp_err_t esp_ota_abort(esp_ota_handle_t h) {
    (void)h;
    s_open--;
    return ESP_OK;
}
esp_err_t esp_ota_set_boot_partition(const esp_partition_t *p) {
    s_boot_set += p == &s_slot;
    return ESP_OK;
}
const char *esp_err_to_name(esp_err_t e) { return e ? "error" : "ESP_OK"; }
void esp_restart(void) { s_restarts++; }

struct fake_timer {
    esp_timer_create_args_t a;
    int armed;
    uint64_t us;
};
static struct fake_timer s_timers[4];
static int s_n_timers;
esp_err_t esp_timer_create(const esp_timer_create_args_t *a, esp_timer_handle_t *out) {
    s_timers[s_n_timers].a = *a;
    *out = &s_timers[s_n_timers++];
    return ESP_OK;
}
esp_err_t esp_timer_start_once(esp_timer_handle_t t, uint64_t us) {
    t->armed = 1;
    t->us = us;
    return ESP_OK;
}
esp_err_t esp_timer_stop(esp_timer_handle_t t) {
    t->armed = 0;
    return ESP_OK;
}
static struct fake_timer *timer(const char *name) {
    for (int i = 0; i < s_n_timers; i++) {
        if (!strcmp(s_timers[i].a.name, name)) return &s_timers[i];
    }
    return NULL;
}

static int s_mutex;
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return &s_mutex; }
int xSemaphoreTake(SemaphoreHandle_t s, uint32_t t) {
    (void)t;
    if (*(int *)s) { printf("FAIL: lock taken twice\n"); failures++; }
    *(int *)s = 1;
    return 1;
}
int xSemaphoreGive(SemaphoreHandle_t s) { *(int *)s = 0; return 1; }

void ota_report_progress(int pct) { s_last_pct = pct; }

// SHA-256 (FIPS 180-4), for the PSA fake.
static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
#define ROR(x, n) ((x) >> (n) | (x) << (32 - (n)))
static void block(psa_hash_operation_t *o, const uint8_t *p) {
    uint32_t w[64], a[8];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4*i] << 24 | p[4*i+1] << 16 | p[4*i+2] << 8 | p[4*i+3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i-15], 7) ^ ROR(w[i-15], 18) ^ (w[i-15] >> 3);
        uint32_t s1 = ROR(w[i-2], 17) ^ ROR(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    memcpy(a, o->h, sizeof(a));
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = a[7] + (ROR(a[4], 6) ^ ROR(a[4], 11) ^ ROR(a[4], 25))
                      + ((a[4] & a[5]) ^ (~a[4] & a[6])) + K[i] + w[i];
        uint32_t t2 = (ROR(a[0], 2) ^ ROR(a[0], 13) ^ ROR(a[0], 22))
                      + ((a[0] & a[1]) ^ (a[0] & a[2]) ^ (a[1] & a[2]));
        memmove(a + 1, a, 7 * sizeof(uint32_t));
        a[4] += t1;
        a[0] = t1 + t2;
    }
    for (int i = 0; i < 8; i++) o->h[i] += a[i];
}
psa_status_t psa_crypto_init(void) { return PSA_SUCCESS; }
psa_status_t psa_hash_setup(psa_hash_operation_t *o, int alg) {
    static const uint32_t h0[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    if (alg != PSA_ALG_SHA_256 || o->active) return -1;
    memcpy(o->h, h0, sizeof(h0));
    o->len = 0;
    o->n = 0;
    o->active = 1;
    return PSA_SUCCESS;
}
psa_status_t psa_hash_update(psa_hash_operation_t *o, const uint8_t *d, size_t n) {
    if (!o->active) return -1;
    o->len += n;
    while (n--) {
        o->buf[o->n++] = *d++;
        if (o->n == 64) { block(o, o->buf); o->n = 0; }
    }
    return PSA_SUCCESS;
}
psa_status_t psa_hash_finish(psa_hash_operation_t *o, uint8_t *out, size_t size, size_t *len) {
    if (!o->active || size < 32) return -1;
    uint64_t bits = o->len * 8;
    uint8_t pad = 0x80, zero = 0;
    psa_hash_update(o, &pad, 1);
    while (o->n != 56) psa_hash_update(o, &zero, 1);
    for (int i = 7; i >= 0; i--) { uint8_t b = (uint8_t)(bits >> (8 * i)); psa_hash_update(o, &b, 1); }
    for (int i = 0; i < 8; i++) for (int j = 0; j < 4; j++) out[4*i+j] = (uint8_t)(o->h[i] >> (24 - 8*j));
    *len = 32;
    o->active = 0;
    return PSA_SUCCESS;
}
psa_status_t psa_hash_abort(psa_hash_operation_t *o) { o->active = 0; return PSA_SUCCESS; }

// ---- helpers ----------------------------------------------------------------

static char *b64(const uint8_t *d, size_t n) {
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char *s = malloc((n + 2) / 3 * 4 + 1), *o = s;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)d[i] << 16 | (i + 1 < n ? d[i+1] << 8 : 0) | (i + 2 < n ? d[i+2] : 0);
        *o++ = T[v >> 18 & 63];
        *o++ = T[v >> 12 & 63];
        *o++ = i + 1 < n ? T[v >> 6 & 63] : '=';
        *o++ = i + 2 < n ? T[v & 63] : '=';
    }
    *o = 0;
    return s;
}

static bool write_at(uint32_t off, const uint8_t *d, size_t n, uint32_t *next, const char **err) {
    char *s = b64(d, n);
    bool ok = ota_push_write(off, s, next, err);
    free(s);
    return ok;
}

static void sha_hex(const uint8_t *d, size_t n, char out[65]) {
    psa_hash_operation_t o = PSA_HASH_OPERATION_INIT;
    uint8_t h[32];
    size_t len;
    psa_hash_setup(&o, PSA_ALG_SHA_256);
    psa_hash_update(&o, d, n);
    psa_hash_finish(&o, h, 32, &len);
    for (int i = 0; i < 32; i++) sprintf(out + 2 * i, "%02x", h[i]);
}

// ---- scenarios --------------------------------------------------------------

int main(int argc, char **argv) {
    // argv[1]: the SHA-256 of "abc" from Python, to check the fake itself.
    char hex[65];
    sha_hex((const uint8_t *)"abc", 3, hex);
    CHECK(argc > 1 && !strcmp(hex, argv[1]));

    // An image: random bytes with an app description at offset 32.
    enum { SIZE = 3 * OTA_PUSH_CHUNK_MAX + 1234 };
    static uint8_t img[SIZE];
    srand(1);
    for (int i = 0; i < SIZE; i++) img[i] = (uint8_t)rand();
    memset(img + 32, 0, 80);
    memcpy(img + 32 + 16, "1.6.0-test", 10);
    sha_hex(img, SIZE, hex);

    uint32_t next = 99;
    const char *err = NULL, *ver = NULL;

    // Nothing to write to before begin; bad parameters.
    CHECK(!write_at(0, img, 100, &next, &err) && strstr(err, "ota.begin"));
    CHECK(!ota_push_begin(SIZE, "abc", &next, &err));
    CHECK(!ota_push_begin(SLOT_SIZE + 1, hex, &next, &err));
    CHECK(!ota_push_finish(&ver, &err));

    CHECK(ota_push_begin(SIZE, hex, &next, &err) && next == 0 && s_last_pct == 0);
    CHECK(timer("ota_push") && timer("ota_push")->armed);
    // Out of order, too big, not base64.
    CHECK(!write_at(100, img, 100, &next, &err) && next == 0);
    static uint8_t big[OTA_PUSH_CHUNK_MAX + 3];
    CHECK(!write_at(0, big, sizeof(big), &next, &err) && strstr(err, "too large"));
    char junk[] = "not*base64";
    CHECK(!ota_push_write(0, junk, &next, &err) && next == 0);

    // Two chunks, then Muse loses track and begins again: it resumes.
    CHECK(write_at(0, img, OTA_PUSH_CHUNK_MAX, &next, &err) && next == OTA_PUSH_CHUNK_MAX);
    CHECK(write_at(next, img + next, OTA_PUSH_CHUNK_MAX, &next, &err)
          && next == 2 * OTA_PUSH_CHUNK_MAX);
    CHECK(!write_at(0, img, OTA_PUSH_CHUNK_MAX, &next, &err)
          && next == 2 * OTA_PUSH_CHUNK_MAX);  // a repeated chunk is refused
    CHECK(ota_push_begin(SIZE, hex, &next, &err) && next == 2 * OTA_PUSH_CHUNK_MAX);
    CHECK(s_open == 1);
    CHECK(!ota_push_finish(&ver, &err) && strstr(err, "not all"));
    // The last chunk can't run past the size.
    CHECK(write_at(next, img + next, OTA_PUSH_CHUNK_MAX, &next, &err)
          && next == 3 * OTA_PUSH_CHUNK_MAX);
    static uint8_t tail[OTA_PUSH_CHUNK_MAX];  // the rest, then some
    memcpy(tail, img + next, SIZE - next);
    CHECK(!write_at(next, tail, sizeof(tail), &next, &err)
          && strstr(err, "past") && next == 3 * OTA_PUSH_CHUNK_MAX);
    while (next < SIZE) {
        size_t n = SIZE - next < OTA_PUSH_CHUNK_MAX ? SIZE - next : OTA_PUSH_CHUNK_MAX;
        if (!write_at(next, img + next, n, &next, &err)) { CHECK(0); break; }
    }
    CHECK(s_last_pct == 100);
    uint32_t sz, nx;
    CHECK(ota_push_status(&sz, &nx) && sz == SIZE && nx == SIZE);
    CHECK(ota_push_finish(&ver, &err) && !strcmp(ver, "1.6.0-test"));
    CHECK(s_flash_n == SIZE && !memcmp(s_flash, img, SIZE));
    CHECK(s_boot_set == 1 && s_open == 0);
    struct fake_timer *rb = timer("ota_reboot");
    CHECK(rb && rb->armed && rb->us == 3000000);
    if (rb) rb->a.callback(rb->a.arg);
    CHECK(s_restarts == 1);
    CHECK(!ota_push_status(&sz, &nx));

    // A corrupted image: the SHA-256 doesn't match, nothing is switched.
    CHECK(ota_push_begin(SIZE, hex, &next, &err) && next == 0);
    img[5000] ^= 1;
    while (next < SIZE) {
        size_t n = SIZE - next < OTA_PUSH_CHUNK_MAX ? SIZE - next : OTA_PUSH_CHUNK_MAX;
        if (!write_at(next, img + next, n, &next, &err)) { CHECK(0); break; }
    }
    CHECK(!ota_push_finish(&ver, &err) && strstr(err, "sha256") && s_last_pct == -1);
    CHECK(s_boot_set == 1 && s_open == 0);

    // An image the bootloader check rejects.
    img[5000] ^= 1;
    s_end_result = ESP_ERR_OTA_VALIDATE_FAILED;
    CHECK(ota_push_begin(SIZE, hex, &next, &err));
    while (next < SIZE) {
        size_t n = SIZE - next < OTA_PUSH_CHUNK_MAX ? SIZE - next : OTA_PUSH_CHUNK_MAX;
        if (!write_at(next, img + next, n, &next, &err)) { CHECK(0); break; }
    }
    CHECK(!ota_push_finish(&ver, &err) && strstr(err, "verification"));
    CHECK(s_boot_set == 1 && s_open == 0 && s_last_pct == -1);
    s_end_result = ESP_OK;

    // A sender that goes quiet: the idle timer abandons the update.
    CHECK(ota_push_begin(SIZE, hex, &next, &err));
    CHECK(write_at(0, img, 100, &next, &err) && next == 100);
    struct fake_timer *idle = timer("ota_push");
    CHECK(idle && idle->armed && idle->us == 120000000ull);
    if (idle) idle->a.callback(idle->a.arg);
    CHECK(!ota_push_status(&sz, &nx) && s_open == 0 && s_last_pct == -1);
    CHECK(!write_at(100, img + 100, 100, &next, &err));

    // A different image replaces one in progress; abort ends it.
    CHECK(ota_push_begin(SIZE, hex, &next, &err) && write_at(0, img, 64, &next, &err));
    char other[65];
    memcpy(other, hex, 65);
    other[0] = other[0] == 'a' ? 'b' : 'a';
    CHECK(ota_push_begin(SIZE, other, &next, &err) && next == 0 && s_open == 1);
    ota_push_abort();
    CHECK(s_open == 0 && !ota_push_status(&sz, &nx));

    printf("%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
