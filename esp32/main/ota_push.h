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

/*
 * Firmware update pushed by Muse in chunks over the encrypted session, for
 * boards without the RAM for a second TLS connection (device.ota).
 *
 *   ota.begin  {size, sha256}        -> {next, chunk_max}
 *   ota.write  {offset, data}        -> {next}       (data: base64)
 *   ota.finish {}                    -> {version}    (then reboots)
 *   ota.abort  {} / ota.status {}
 *
 * Chunks go straight into the inactive OTA slot. ota.begin with the same size
 * and sha256 as an update in progress resumes it (its `next` says where), and
 * a write at any offset other than `next` is refused with `next` in the error,
 * so a sender can always pick up where the board is. ota.finish checks the
 * SHA-256 of what arrived, verifies the image and its signature, switches the
 * boot slot and reboots; the usual rollback rules then apply. An update that
 * goes 2 minutes without a chunk is abandoned.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Largest decoded chunk ota.write takes: base64 of it plus the invoke
// envelope must fit the session's inbound frame (8 KB without PSRAM).
#define OTA_PUSH_CHUNK_MAX 4096

// Start (or resume) an update of `size` bytes whose SHA-256 is `sha256_hex`.
// *next receives the offset to write next. False with *err on failure.
bool ota_push_begin(uint32_t size, const char *sha256_hex, uint32_t *next,
                    const char **err);

// Write base64 `b64` at `offset`, decoding in place (the buffer is
// modified). *next receives the offset expected next, also on a refused write.
bool ota_push_write(uint32_t offset, char *b64, uint32_t *next, const char **err);

// Check and apply the update: on success *version is the new firmware's
// version and the board reboots in a few seconds.
bool ota_push_finish(const char **version, const char **err);

// Abandon an update in progress (the inactive slot keeps whatever was written).
void ota_push_abort(void);

// Whether an update is in progress, and its size and the offset reached.
bool ota_push_status(uint32_t *size, uint32_t *next);

#ifdef __cplusplus
}
#endif
