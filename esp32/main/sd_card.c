// sd_card.c — SD card via SDSPI on its own SPI bus (SPI3_HOST / VSPI).
//
// On the ESP32-2432S028R the SD slot is wired to the VSPI pins (SCK 18,
// MOSI 23, MISO 19, CS 5), not the display's bus (HSPI: 14/13/12), and the
// display bus has no MISO configured at all. So the card gets SPI3_HOST.

#include "sd_card.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"

static const char *TAG = "sd.card";
static sdmmc_card_t *s_card;
static bool s_mounted;
static bool s_bus_ready;

#define SD_HOST SPI3_HOST

#define MOUNT_POINT "/sdcard"

bool sd_card_init(void) {
    if (s_mounted) return true;

    ESP_LOGI(TAG, "probing SD card (SCK=%d MOSI=%d MISO=%d CS=%d)",
             CONFIG_HOMEHUB_SDCARD_SCK, CONFIG_HOMEHUB_SDCARD_MOSI,
             CONFIG_HOMEHUB_SDCARD_MISO, CONFIG_HOMEHUB_SDCARD_CS);

    if (!s_bus_ready) {
        spi_bus_config_t bus = {
            .sclk_io_num = CONFIG_HOMEHUB_SDCARD_SCK,
            .mosi_io_num = CONFIG_HOMEHUB_SDCARD_MOSI,
            .miso_io_num = CONFIG_HOMEHUB_SDCARD_MISO,
            .quadwp_io_num = -1,
            .quadhd_io_num = -1,
            .max_transfer_sz = 4000,
        };
        esp_err_t err = spi_bus_initialize(SD_HOST, &bus, SPI_DMA_CH_AUTO);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "SD SPI bus init failed: %s", esp_err_to_name(err));
            return false;
        }
        s_bus_ready = true;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SD_HOST;

    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = CONFIG_HOMEHUB_SDCARD_CS;
    slot.host_id = SD_HOST;

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files = 4,
        .allocation_unit_size = 16 * 1024,
    };

    esp_err_t err = esp_vfs_fat_sdspi_mount(MOUNT_POINT, &host, &slot,
                                           &mount_cfg, &s_card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SD mount failed: %s (%s)",
                 esp_err_to_name(err),
                 err == ESP_ERR_NOT_FOUND ? "no card?" : "check wiring/CS pin");
        s_card = NULL;
        return false;
    }

    s_mounted = true;
    char info[64];
    sd_card_info(info, sizeof(info));
    ESP_LOGI(TAG, "mounted: %s", info);
    return true;
}

bool sd_card_mounted(void) {
    return s_mounted;
}

void sd_card_info(char *buf, size_t n) {
    if (!s_mounted || !s_card) {
        snprintf(buf, n, "not mounted");
        return;
    }
    uint64_t bytes = (uint64_t)s_card->csd.capacity * s_card->csd.sector_size;
    snprintf(buf, n, "%llu MB %s", bytes / (1024 * 1024),
             s_card->is_sdio ? "SDIO" :
             s_card->is_mmc ? "MMC" : "SD");
}

void sd_card_deinit(void) {
    if (!s_mounted) return;
    esp_vfs_fat_sdcard_unmount(MOUNT_POINT, s_card);
    s_card = NULL;
    s_mounted = false;
    ESP_LOGI(TAG, "unmounted");
}
