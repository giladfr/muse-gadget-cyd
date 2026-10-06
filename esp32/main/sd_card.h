// sd_card.h — SD card via SDSPI on the shared display SPI bus.
#pragma once

#include <stdbool.h>
#include <stddef.h>

// Mount the SD card at /sdcard. Returns true on success. Safe to call when
// no card is present (returns false, logs a warning).
bool sd_card_init(void);

// True if a card is mounted.
bool sd_card_mounted(void);

// One-line card info for diagnostics (size, type). buf must hold ~64 bytes.
void sd_card_info(char *buf, size_t n);

// Unmount (e.g. before OTA).
void sd_card_deinit(void);
