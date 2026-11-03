#include "c2_client.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "tinyusb.h"
#include "tinyusb_cdc_acm.h"
#include "c2_protocol.h"

extern QueueHandle_t s_c2_queue;

static const char *TAG = "c2_client";

static SemaphoreHandle_t s_c2_mutex = NULL;

#define C2_EXEC_TIMEOUT_MS    60000
#define C2_UPLOAD_ACK_MS      10000
#define C2_UPLOAD_CHUNK_SIZE  1024

/* Timeout download basati su INATTIVITÀ (non wall-clock).
 * C2_DOWNLOAD_IDLE_MS: se non arriva alcun chunk/RC per 30s -> timeout.
 * C2_DOWNLOAD_ABORT_MS: dopo un abort del chunk_cb, invio ABORT_DOWNLOAD
 *                       all'agente e attendo il suo DOWNLOAD_RC (rc=2).
 *                       L'agente ha un polling ABORT ogni ~64 KB, quindi
 *                       risponde in poche centinaia di ms; 5s sono un
 *                       margine di sicurezza generoso. Se scatta, è un
 *                       errore grave (l'agente non risponde all'abort). */
#define C2_DOWNLOAD_IDLE_MS   30000
#define C2_DOWNLOAD_ABORT_MS   5000

esp_err_t c2_client_init(void)
{
    if (s_c2_mutex == NULL) {
        s_c2_mutex = xSemaphoreCreateMutex();
        if (s_c2_mutex == NULL) {
            ESP_LOGE(TAG, "Failed to create C2 mutex");
            return ESP_ERR_NO_MEM;
        }
    }
    ESP_LOGI(TAG, "C2 client initialized");
    return ESP_OK;
}

static bool cdc_write_all(const uint8_t *buf, size_t total)
{
    size_t written = 0;
    int stall = 0;
    while (written < total) {
        size_t remain = total - written;
        int q = tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0,
                                            (uint8_t *)(buf + written),
                                            remain);
        if (q < 0) {
            ESP_LOGE(TAG, "cdc_write: queue error %d", q);
            return false;
        }
        if (q == 0) {
            esp_err_t f = tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0,
                                                      pdMS_TO_TICKS(100));
            if (f != ESP_OK) {
                ESP_LOGW(TAG, "cdc_write: flush error %s", esp_err_to_name(f));
            }
            vTaskDelay(pdMS_TO_TICKS(2));
            if (++stall > 100) {
                ESP_LOGE(TAG, "cdc_write: stall after %u/%u",
                         (unsigned)written, (unsigned)total);
                return false;
            }
            continue;
        }
        written += q;
        stall = 0;
        if (written < total) {
            tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, pdMS_TO_TICKS(100));
        }
    }
    tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, pdMS_TO_TICKS(500));
    return true;
}

static esp_err_t c2_execute_common(const char *cmd, char *out, size_t out_max,
                                   size_t *out_len, c2_chunk_cb_t cb, void *user_ctx)
{
    if (!cmd) return ESP_ERR_INVALID_ARG;
    if (!tud_mounted()) return ESP_ERR_INVALID_STATE;
    if (!s_c2_queue) return ESP_ERR_INVALID_STATE;
    if (s_c2_mutex == NULL) return ESP_ERR_INVALID_STATE;

    if (xSemaphoreTake(s_c2_mutex, pdMS_TO_TICKS(65000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    xQueueReset(s_c2_queue);

    size_t n = strlen(cmd);
    if (n > 500) {
        xSemaphoreGive(s_c2_mutex);
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t frame[512];
    frame[0] = C2_TAG_EXEC;
    uint32_t len = (uint32_t)n;
    frame[1] = len & 0xFF;
    frame[2] = (len >> 8) & 0xFF;
    frame[3] = (len >> 16) & 0xFF;
    frame[4] = (len >> 24) & 0xFF;
    memcpy(frame + 5, cmd, n);

    ESP_LOGI(TAG, "exec: cmd='%s'", cmd);

    if (!cdc_write_all(frame, 5 + n)) {
        ESP_LOGE(TAG, "exec: write failed");
        xSemaphoreGive(s_c2_mutex);
        return ESP_FAIL;
    }

    size_t total_out = 0;
    bool got_rc = false;
    bool truncated = false;

    c2_msg_t m;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(C2_EXEC_TIMEOUT_MS);

    while (!got_rc && xTaskGetTickCount() < deadline) {
        if (xQueueReceive(s_c2_queue, &m, pdMS_TO_TICKS(200)) == pdTRUE) {
            if (m.tag == C2_TAG_STDOUT || m.tag == C2_TAG_STDERR) {
                if (cb) {
                    cb(m.data, m.len, user_ctx);
                } else if (out && out_max > 0) {
                    if (total_out + m.len <= out_max - 1) {
                        memcpy(out + total_out, m.data, m.len);
                        total_out += m.len;
                    } else {
                        truncated = true;
                    }
                }
            } else if (m.tag == C2_TAG_RC) {
                got_rc = true;
                if (cb) cb(NULL, 0, user_ctx);
            }
        }
    }

    if (out && out_max > 0) {
        out[total_out] = '\0';
        if (out_len) *out_len = total_out;
    }

    ESP_LOGI(TAG, "exec: done rc=%d, %u bytes%s",
             got_rc, (unsigned)total_out, truncated ? " (TRUNCATED)" : "");

    xSemaphoreGive(s_c2_mutex);
    return ESP_OK;
}

esp_err_t c2_execute_command(const char *cmd, char *out, size_t out_max, size_t *out_len)
{
    return c2_execute_common(cmd, out, out_max, out_len, NULL, NULL);
}

esp_err_t c2_execute_command_stream(const char *cmd, c2_chunk_cb_t cb, void *user_ctx)
{
    if (!cb) return ESP_ERR_INVALID_ARG;
    return c2_execute_common(cmd, NULL, 0, NULL, cb, user_ctx);
}

static int wait_upload_rc(TickType_t timeout)
{
    c2_msg_t m;
    TickType_t deadline = xTaskGetTickCount() + timeout;
    while (xTaskGetTickCount() < deadline) {
        if (xQueueReceive(s_c2_queue, &m, pdMS_TO_TICKS(200)) == pdTRUE) {
            if (m.tag == C2_TAG_UPLOAD_RC && m.len >= 4) {
                int rc = (int)((uint32_t)m.data[0]
                             | ((uint32_t)m.data[1] << 8)
                             | ((uint32_t)m.data[2] << 16)
                             | ((uint32_t)m.data[3] << 24));
                return rc;
            }
        }
    }
    return -1;
}

esp_err_t c2_upload_file(const char *path, size_t total_size,
                         c2_read_cb_t read_cb, void *user_ctx,
                         size_t *bytes_sent)
{
    if (!path || !read_cb || !bytes_sent) return ESP_ERR_INVALID_ARG;
    if (!tud_mounted()) return ESP_ERR_INVALID_STATE;
    if (!s_c2_queue || !s_c2_mutex) return ESP_ERR_INVALID_STATE;

    *bytes_sent = 0;

    if (xSemaphoreTake(s_c2_mutex, pdMS_TO_TICKS(65000)) != pdTRUE) {
        ESP_LOGE(TAG, "upload: mutex timeout");
        return ESP_ERR_TIMEOUT;
    }

    xQueueReset(s_c2_queue);

    size_t path_len = strlen(path);
    if (path_len > 500) {
        xSemaphoreGive(s_c2_mutex);
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t frame[512];
    frame[0] = C2_TAG_UPLOAD_START;
    uint32_t len = (uint32_t)path_len;
    frame[1] = len & 0xFF;
    frame[2] = (len >> 8) & 0xFF;
    frame[3] = (len >> 16) & 0xFF;
    frame[4] = (len >> 24) & 0xFF;
    memcpy(frame + 5, path, path_len);

    ESP_LOGI(TAG, "upload: start path='%s' size=%u", path, (unsigned)total_size);

    if (!cdc_write_all(frame, 5 + path_len)) {
        ESP_LOGE(TAG, "upload: start write failed");
        xSemaphoreGive(s_c2_mutex);
        return ESP_FAIL;
    }

    int rc = wait_upload_rc(pdMS_TO_TICKS(C2_UPLOAD_ACK_MS));
    if (rc != 0) {
        ESP_LOGE(TAG, "upload: start rc=%d", rc);
        xSemaphoreGive(s_c2_mutex);
        return ESP_FAIL;
    }

    uint8_t chunk[C2_UPLOAD_CHUNK_SIZE];
    uint8_t f[5 + C2_UPLOAD_CHUNK_SIZE];
    size_t total = 0;
    bool error = false;

    while (true) {
        size_t n = read_cb(chunk, sizeof(chunk), user_ctx);
        if (n == 0) break;

        f[0] = C2_TAG_UPLOAD_CHUNK;
        uint32_t clen = (uint32_t)n;
        f[1] = clen & 0xFF;
        f[2] = (clen >> 8) & 0xFF;
        f[3] = (clen >> 16) & 0xFF;
        f[4] = (clen >> 24) & 0xFF;
        memcpy(f + 5, chunk, n);

        if (!cdc_write_all(f, 5 + n)) {
            ESP_LOGE(TAG, "upload: chunk write failed at %u", (unsigned)total);
            error = true;
            break;
        }

        total += n;
        if ((total % (64 * 1024)) < n) {
            ESP_LOGI(TAG, "upload: %u bytes sent", (unsigned)total);
        }
    }

    uint8_t end_frame[5] = { C2_TAG_UPLOAD_END, 0, 0, 0, 0 };
    if (!cdc_write_all(end_frame, 5)) {
        ESP_LOGE(TAG, "upload: end write failed");
        error = true;
    }

    int final_rc = wait_upload_rc(pdMS_TO_TICKS(C2_EXEC_TIMEOUT_MS));
    if (final_rc != 0) {
        ESP_LOGE(TAG, "upload: final rc=%d", final_rc);
        error = true;
    }

    *bytes_sent = total;
    ESP_LOGI(TAG, "upload: done %u bytes, rc=%d%s",
             (unsigned)total, final_rc, error ? " (ERROR)" : "");

    xSemaphoreGive(s_c2_mutex);
    return error ? ESP_FAIL : ESP_OK;
}

/* Invia il comando di abort all'agente. L'agente ha un polling
 * ABORT ogni ~64 KB dentro do_download(): lo intercetta in poche
 * centinaia di ms a 100 KB/s, poi invia DOWNLOAD_RC con rc=2. */
static bool send_abort_download(void)
{
    uint8_t frame[5] = { C2_TAG_ABORT_DOWNLOAD, 0, 0, 0, 0 };
    return cdc_write_all(frame, 5);
}

esp_err_t c2_download_file(const char *path,
                           c2_download_chunk_cb_t chunk_cb, void *user_ctx,
                           size_t *total_bytes, int *rc_out)
{
    if (!path || !chunk_cb || !total_bytes || !rc_out) return ESP_ERR_INVALID_ARG;
    if (!tud_mounted()) return ESP_ERR_INVALID_STATE;
    if (!s_c2_queue || !s_c2_mutex) return ESP_ERR_INVALID_STATE;

    *total_bytes = 0;
    *rc_out = -1;

    if (xSemaphoreTake(s_c2_mutex, pdMS_TO_TICKS(65000)) != pdTRUE) {
        ESP_LOGE(TAG, "download: mutex timeout");
        return ESP_ERR_TIMEOUT;
    }

    xQueueReset(s_c2_queue);

    size_t path_len = strlen(path);
    if (path_len > 500) {
        xSemaphoreGive(s_c2_mutex);
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t frame[512];
    frame[0] = C2_TAG_DOWNLOAD_START;
    uint32_t len = (uint32_t)path_len;
    frame[1] = len & 0xFF;
    frame[2] = (len >> 8) & 0xFF;
    frame[3] = (len >> 16) & 0xFF;
    frame[4] = (len >> 24) & 0xFF;
    memcpy(frame + 5, path, path_len);

    ESP_LOGI(TAG, "download: start path='%s'", path);

    if (!cdc_write_all(frame, 5 + path_len)) {
        ESP_LOGE(TAG, "download: start write failed");
        xSemaphoreGive(s_c2_mutex);
        return ESP_FAIL;
    }

    size_t total = 0;
    int final_rc = -1;
    bool done = false;
    bool aborted = false;
    bool abort_sent = false;
    TickType_t last_activity = xTaskGetTickCount();
    TickType_t abort_start = 0;
    TickType_t idle_ticks  = pdMS_TO_TICKS(C2_DOWNLOAD_IDLE_MS);
    TickType_t abort_ticks = pdMS_TO_TICKS(C2_DOWNLOAD_ABORT_MS);

    while (!done) {
        TickType_t now = xTaskGetTickCount();

        /* Timeout per inattività: nessun messaggio per 30s */
        if ((now - last_activity) > idle_ticks) {
            ESP_LOGW(TAG, "download: idle timeout (%u bytes)", (unsigned)total);
            break;
        }
        /* Se siamo in drain post-abort, limitiamo l'attesa del
         * DOWNLOAD_RC a C2_DOWNLOAD_ABORT_MS (l'agente risponde
         * subito, quindi in pratica si esce prima). */
        if (aborted && (now - abort_start) > abort_ticks) {
            ESP_LOGW(TAG, "download: abort drain TIMEOUT (%u bytes) — DESYNC RISK",
                     (unsigned)total);
            break;
        }

        c2_msg_t m;
        if (xQueueReceive(s_c2_queue, &m, pdMS_TO_TICKS(200)) != pdTRUE) {
            continue;
        }
        last_activity = xTaskGetTickCount();

        if (m.tag == C2_TAG_DOWNLOAD_CHUNK) {
            if (aborted) {
                /* drain silenzioso: skip */
            } else if (!chunk_cb(m.data, m.len, user_ctx)) {
                ESP_LOGW(TAG, "download: chunk_cb ha abortito, invio ABORT_DOWNLOAD");
                aborted = true;
                abort_start = xTaskGetTickCount();
                if (send_abort_download()) {
                    abort_sent = true;
                } else {
                    ESP_LOGE(TAG, "download: send ABORT_DOWNLOAD fallita");
                }
            } else {
                total += m.len;
            }
        } else if (m.tag == C2_TAG_DOWNLOAD_END) {
            /* marcatore legacy: ignorato, aspettiamo DOWNLOAD_RC */
        } else if (m.tag == C2_TAG_DOWNLOAD_RC && m.len >= 4) {
            final_rc = (int)((uint32_t)m.data[0]
                           | ((uint32_t)m.data[1] << 8)
                           | ((uint32_t)m.data[2] << 16)
                           | ((uint32_t)m.data[3] << 24));
            done = true;
        }
    }

    *total_bytes = total;
    *rc_out = final_rc;

    ESP_LOGI(TAG, "download: done %u bytes, rc=%d%s%s%s",
             (unsigned)total, final_rc,
             done       ? "" : " (TIMEOUT)",
             aborted    ? " (ABORTED)" : "",
             (aborted && !abort_sent) ? " (abort not sent)" : "");

    xSemaphoreGive(s_c2_mutex);
    return done ? ESP_OK : ESP_ERR_TIMEOUT;
}