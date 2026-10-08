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

// Host fake for tests/test_ota_push.py.
#pragma once
#include <stddef.h>
#include <stdint.h>
typedef int psa_status_t;
#define PSA_SUCCESS 0
#define PSA_ALG_SHA_256 1
typedef struct {
    uint32_t h[8];
    uint64_t len;
    uint8_t buf[64];
    size_t n;
    int active;
} psa_hash_operation_t;
#define PSA_HASH_OPERATION_INIT {{0}, 0, {0}, 0, 0}
psa_status_t psa_crypto_init(void);
psa_status_t psa_hash_setup(psa_hash_operation_t *op, int alg);
psa_status_t psa_hash_update(psa_hash_operation_t *op, const uint8_t *d, size_t n);
psa_status_t psa_hash_finish(psa_hash_operation_t *op, uint8_t *out, size_t size, size_t *len);
psa_status_t psa_hash_abort(psa_hash_operation_t *op);
