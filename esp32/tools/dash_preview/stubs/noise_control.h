#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
bool noise_ctrl_is_connected(void);
typedef void (*noise_ctrl_req_cb)(void *ctx, int status, const uint8_t *data, size_t len, bool end);
int64_t noise_ctrl_req_open(const char *verb, const char *path, const char *const *headers, bool end_body, noise_ctrl_req_cb cb, void *ctx);
bool noise_ctrl_req_send(int64_t id, const void *data, size_t len, bool end_body, int wait_ms);
void noise_ctrl_req_cancel(int64_t id);
