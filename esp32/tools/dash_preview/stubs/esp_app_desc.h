// Host stub: the app description the boot splash shows.
#pragma once

typedef struct {
    char version[32];
} esp_app_desc_t;

#ifndef DASH_HOST_VERSION
#define DASH_HOST_VERSION "host"
#endif

static inline const esp_app_desc_t *esp_app_get_description(void) {
    static const esp_app_desc_t desc = {DASH_HOST_VERSION};
    return &desc;
}
