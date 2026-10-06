// sd_fetch.h — download a file from a URL onto the SD card, over the air.
#pragma once

#include <stdbool.h>
#include <stddef.h>

// Called on the download task when it finishes. err is NULL on success.
typedef void (*sd_fetch_done_cb)(const char *err, size_t bytes, void *user);

// Start downloading `url` to /sdcard/<path>. The file is written to
// <path>.part and renamed into place only when complete (and, if
// `sha256_hex` is given, only when the SHA-256 matches), so a failed or
// interrupted download never leaves a half-written asset behind. Plain HTTP
// is recommended on this board (no PSRAM for TLS). Returns false with *err
// if it could not start; otherwise `cb` reports the outcome.
bool sd_fetch_start(const char *url, const char *path, const char *sha256_hex,
                    sd_fetch_done_cb cb, void *user, const char **err);
