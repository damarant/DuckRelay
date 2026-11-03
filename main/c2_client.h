#pragma once

#include <stddef.h>
#include "esp_err.h"

esp_err_t c2_client_init(void);

esp_err_t c2_execute_command(const char *cmd, char *out, size_t out_max, size_t *out_len);

typedef void (*c2_chunk_cb_t)(const uint8_t *data, size_t len, void *user_ctx);
esp_err_t c2_execute_command_stream(const char *cmd, c2_chunk_cb_t cb, void *user_ctx);

typedef size_t (*c2_read_cb_t)(uint8_t *buf, size_t max_len, void *user_ctx);
esp_err_t c2_upload_file(const char *path, size_t total_size,
                         c2_read_cb_t read_cb, void *user_ctx,
                         size_t *bytes_sent);

typedef bool (*c2_download_chunk_cb_t)(const uint8_t *data, size_t len, void *user_ctx);
esp_err_t c2_download_file(const char *path,
                           c2_download_chunk_cb_t chunk_cb, void *user_ctx,
                           size_t *total_bytes, int *rc_out);