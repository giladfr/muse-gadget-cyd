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

// Weak fakes for what main/ota.c's progress reporting and crash-loop guard
// call; a harness that cares defines its own.
#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "esp_system.h"

__attribute__((weak)) int esp_https_ota_get_image_size(esp_https_ota_handle_t h) {
    (void)h;
    return -1;
}
__attribute__((weak)) int esp_https_ota_get_image_len_read(esp_https_ota_handle_t h) {
    (void)h;
    return 0;
}
__attribute__((weak)) esp_reset_reason_t esp_reset_reason(void) {
    return ESP_RST_POWERON;
}
__attribute__((weak)) const esp_partition_t *esp_ota_get_running_partition(void) {
    return NULL;
}
__attribute__((weak)) const esp_partition_t *esp_ota_get_next_update_partition(
    const esp_partition_t *start) {
    (void)start;
    return NULL;
}
__attribute__((weak)) esp_err_t esp_ota_get_partition_description(
    const esp_partition_t *partition, esp_app_desc_t *app_desc) {
    (void)partition;
    (void)app_desc;
    return -1;
}
__attribute__((weak)) esp_err_t esp_ota_set_boot_partition(const esp_partition_t *partition) {
    (void)partition;
    return -1;
}
