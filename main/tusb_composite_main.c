/*
 * SPDX-FileCopyrightText: 2022-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include "version.h"
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <string.h>
#include "esp_partition.h"
#include "esp_check.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_msc.h"
#include "tinyusb_cdc_acm.h"
#include "class/hid/hid_device.h"
#include "driver/gpio.h"
#include "payload_parser.h"
#include "wifi_ap.h"
#include "web_server.h"
#include "c2_protocol.h"
#include "c2_client.h"

#define BASE_PATH "/usb"

/* Finestra temporale per distinguere singola/doppia pressione BOOT */
#define BOOT_DOUBLE_PRESS_MS 500

static const char *TAG = "example_main";
static uint8_t rx_buf[CONFIG_TINYUSB_CDC_RX_BUFSIZE + 1];

static QueueHandle_t app_queue;
typedef struct {
    uint8_t buf[CONFIG_TINYUSB_CDC_RX_BUFSIZE + 1];
    size_t buf_len;
    uint8_t itf;
} app_message_t;

static tinyusb_msc_storage_handle_t s_storage_hdl = NULL;

QueueHandle_t s_c2_queue = NULL;

#define C2_ACCUM_SIZE (C2_MSG_MAX + 64)
static uint8_t s_c2_accum[C2_ACCUM_SIZE];
static size_t  s_c2_accum_len = 0;

/* ============================================================
 * HID: report descriptor
 * ============================================================ */
const uint8_t hid_report_descriptor[] = {
    TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(HID_ITF_PROTOCOL_KEYBOARD))
};

#define TUSB_DESC_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN + \
                             TUD_CDC_DESC_LEN + TUD_MSC_DESC_LEN)

static const uint8_t composite_configuration_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(1, 4, 0, TUSB_DESC_TOTAL_LEN, 0, 100),
    TUD_HID_DESCRIPTOR(0, 0, HID_ITF_PROTOCOL_KEYBOARD,
                       sizeof(hid_report_descriptor), 0x81, 16, 10),
    TUD_CDC_DESCRIPTOR(1, 0, 0x83, 8, 0x02, 0x82, 64),
    TUD_MSC_DESCRIPTOR(3, 0, 0x84, 0x04, 64),
};

/* ============================================================
 * HID callbacks
 * ============================================================ */
uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    (void) instance;
    return hid_report_descriptor;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                               hid_report_type_t report_type,
                               uint8_t *buffer, uint16_t reqlen)
{
    (void) instance; (void) report_id; (void) report_type;
    (void) buffer; (void) reqlen;
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type,
                           uint8_t const *buffer, uint16_t bufsize)
{
    (void) instance; (void) report_id; (void) report_type;
    (void) buffer; (void) bufsize;
}

/* ============================================================
 * CDC RX: accumula e parse TLV completo
 * ============================================================ */
void tinyusb_cdc_rx_callback(int itf, cdcacm_event_t *event)
{
    size_t rx_size = 0;
    esp_err_t ret = tinyusb_cdcacm_read(itf, rx_buf,
                                        CONFIG_TINYUSB_CDC_RX_BUFSIZE,
                                        &rx_size);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "CDC read error");
        return;
    }

    if (s_c2_accum_len + rx_size > C2_ACCUM_SIZE) {
        ESP_LOGW(TAG, "C2 accum overflow, reset");
        s_c2_accum_len = 0;
        return;
    }
    memcpy(s_c2_accum + s_c2_accum_len, rx_buf, rx_size);
    s_c2_accum_len += rx_size;

    size_t off = 0;
    while (off + 5 <= s_c2_accum_len && s_c2_queue) {
        uint8_t tag = s_c2_accum[off];
        uint32_t len = (uint32_t)s_c2_accum[off + 1]
                     | ((uint32_t)s_c2_accum[off + 2] << 8)
                     | ((uint32_t)s_c2_accum[off + 3] << 16)
                     | ((uint32_t)s_c2_accum[off + 4] << 24);

        if (off + 5 + len > s_c2_accum_len) break;

        if (len <= sizeof(((c2_msg_t *)0)->data)) {
            c2_msg_t m = {0};
            m.tag = tag;
            m.len = len;
            memcpy(m.data, s_c2_accum + off + 5, len);
            xQueueSend(s_c2_queue, &m, 0);
        }
        off += 5 + len;
    }

    if (off > 0) {
        memmove(s_c2_accum, s_c2_accum + off, s_c2_accum_len - off);
        s_c2_accum_len -= off;
    }
}

void tinyusb_cdc_line_state_changed_callback(int itf, cdcacm_event_t *event)
{
    int dtr = event->line_state_changed_data.dtr;
    int rts = event->line_state_changed_data.rts;
    ESP_LOGI(TAG, "Line state changed on channel %d: DTR:%d, RTS:%d", itf, dtr, rts);
}

/* ============================================================
 * Filesystem helpers
 * ============================================================ */
static bool file_exists(const char *file_path)
{
    struct stat buffer;
    return stat(file_path, &buffer) == 0;
}

static void file_operations(void)
{
    const char *directory = "/usb/esp";
    const char *file_path = "/usb/esp/test.txt";

    struct stat s = {0};
    if (stat(directory, &s) != 0) {
        if (mkdir(directory, 0775) != 0) {
            ESP_LOGE(TAG, "mkdir failed with errno: %s", strerror(errno));
        }
    }

    if (!file_exists(file_path)) {
        ESP_LOGI(TAG, "Creating file");
        FILE *f = fopen(file_path, "w");
        if (f == NULL) {
            ESP_LOGE(TAG, "Failed to open file for writing");
            return;
        }
        fprintf(f, "Hello World!\n");
        fclose(f);
    }

    FILE *f = fopen(file_path, "r");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open file for reading");
        return;
    }
    char line[64];
    fgets(line, sizeof(line), f);
    fclose(f);
    char *pos = strchr(line, '\n');
    if (pos) *pos = '\0';
    ESP_LOGI(TAG, "Read from file: '%s'", line);
}

static esp_err_t storage_init_spiflash(wl_handle_t *wl_handle)
{
    ESP_LOGI(TAG, "Initializing wear levelling");
    const esp_partition_t *data_partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, NULL);
    if (data_partition == NULL) {
        ESP_LOGE(TAG, "Failed to find FATFS partition. Check the partition table.");
        return ESP_ERR_NOT_FOUND;
    }
    return wl_mount(data_partition, wl_handle);
}

/* ============================================================
 * BOOT trigger con deploy automatico
 * ============================================================ */
#define DEPLOY_MAX_SIZE 8192

static const char *TEST_PAYLOAD =
    "DELAY 1000\n"
    "GUI r\n"
    "DELAY 500\n"
    "STRING notepad\n"
    "ENTER\n"
    "DELAY 1000\n"
    "STRING Hello from DuckRelay!\n";

static void run_deploy_sequence(void)
{
    if (!tud_mounted()) {
        ESP_LOGW(TAG, "deploy: USB not mounted, skip");
        return;
    }
    if (!s_storage_hdl) {
        ESP_LOGW(TAG, "deploy: storage handle null, skip");
        return;
    }

    tinyusb_msc_mount_point_t mp;
    tinyusb_msc_get_storage_mount_point(s_storage_hdl, &mp);
    if (mp != TINYUSB_MSC_STORAGE_MOUNT_APP) {
        tinyusb_msc_set_storage_mount_point(s_storage_hdl, TINYUSB_MSC_STORAGE_MOUNT_APP);
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    char *buf = NULL;
    FILE *f = fopen("/usb/deploy.txt", "r");
    if (!f) {
        ESP_LOGW(TAG, "deploy: /usb/deploy.txt non leggibile, fallback TEST_PAYLOAD");
        payload_execute_string(TEST_PAYLOAD);
        return;
    }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > DEPLOY_MAX_SIZE) {
        ESP_LOGE(TAG, "deploy: size invalida (%ld)", sz);
        fclose(f);
        return;
    }
    buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = '\0';
    ESP_LOGI(TAG, "deploy: loaded %u bytes from /usb/deploy.txt", (unsigned)n);

    ESP_LOGI(TAG, "deploy: switch mount -> USB");
    esp_err_t err = tinyusb_msc_set_storage_mount_point(s_storage_hdl,
                                                         TINYUSB_MSC_STORAGE_MOUNT_USB);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "deploy: switch USB failed: %s", esp_err_to_name(err));
        free(buf);
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(3000));

    ESP_LOGI(TAG, "deploy: running payload");
    payload_execute_string(buf);
    free(buf);

    vTaskDelay(pdMS_TO_TICKS(8000));

    ESP_LOGI(TAG, "deploy: switch mount -> APP");
    tinyusb_msc_set_storage_mount_point(s_storage_hdl,
                                        TINYUSB_MSC_STORAGE_MOUNT_APP);

    ESP_LOGI(TAG, "deploy: sequence completed");
}

static void boot_trigger_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "boot_trigger_task started");

    const gpio_config_t io = {
        .pin_bit_mask = (1ULL << 0),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));

    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_LOGI(TAG, "GPIO0 initial level = %d", gpio_get_level(0));

    bool was_pressed = false;
    bool timer_armed = false;        /* prima pressione rilevata, in attesa */
    TickType_t first_press_tick = 0;

    while (1) {
        bool pressed = (gpio_get_level(0) == 0);
        TickType_t now = xTaskGetTickCount();

        /* Fronte di discesa = nuova pressione */
        if (pressed && !was_pressed) {
            if (!timer_armed) {
                /* Prima pressione: arma timer, non fare nulla subito */
                timer_armed = true;
                first_press_tick = now;
                ESP_LOGI(TAG, "BOOT press #1 (armed, waiting %d ms)",
                         BOOT_DOUBLE_PRESS_MS);
            } else {
                /* Seconda pressione entro la finestra: deploy US */
                ESP_LOGI(TAG, "BOOT press #2 -> US deploy");
                timer_armed = false;
                payload_set_layout(false);   /* US */
                run_deploy_sequence();
            }
        }

        /* Se il timer è armato e sono passati >500 ms senza seconda
         * pressione: era una singola pressione -> deploy IT */
        if (timer_armed && (now - first_press_tick) >
                           pdMS_TO_TICKS(BOOT_DOUBLE_PRESS_MS)) {
            ESP_LOGI(TAG, "BOOT single press timeout -> IT deploy");
            timer_armed = false;
            payload_set_layout(true);    /* IT */
            run_deploy_sequence();
        }

        was_pressed = pressed;
        vTaskDelay(pdMS_TO_TICKS(20));   /* poll 20 ms per reattività */
    }
}

/* ============================================================
 * app_main
 * ============================================================ */
void app_main(void)
{
	ESP_LOGI(TAG, "DuckRelay firmware v%s starting", DUCKRELAY_VERSION);
    app_queue = xQueueCreate(5, sizeof(app_message_t));
    assert(app_queue);

    s_c2_queue = xQueueCreate(8, sizeof(c2_msg_t));
    assert(s_c2_queue);

    ESP_LOGI(TAG, "Initializing storage...");

    static wl_handle_t wl_handle = WL_INVALID_HANDLE;
    ESP_ERROR_CHECK(storage_init_spiflash(&wl_handle));

    const tinyusb_msc_storage_config_t storage_cfg = {
        .mount_point = TINYUSB_MSC_STORAGE_MOUNT_APP,
        .medium.wl_handle = wl_handle,
        .fat_fs = { .base_path = BASE_PATH },
    };
    ESP_ERROR_CHECK(tinyusb_msc_new_storage_spiflash(&storage_cfg, &s_storage_hdl));

    file_operations();

    ESP_LOGI(TAG, "USB Composite initialization");
    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    tusb_cfg.descriptor.full_speed_config = composite_configuration_descriptor;
    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));

    tinyusb_config_cdcacm_t acm_cfg = {
        .cdc_port = TINYUSB_CDC_ACM_0,
        .callback_rx = &tinyusb_cdc_rx_callback,
        .callback_rx_wanted_char = NULL,
        .callback_line_state_changed = NULL,
        .callback_line_coding_changed = NULL
    };
    ESP_ERROR_CHECK(tinyusb_cdcacm_init(&acm_cfg));
    ESP_ERROR_CHECK(tinyusb_cdcacm_register_callback(
                        TINYUSB_CDC_ACM_0,
                        CDC_EVENT_LINE_STATE_CHANGED,
                        &tinyusb_cdc_line_state_changed_callback));

    ESP_ERROR_CHECK(payload_parser_init());
    xTaskCreate(boot_trigger_task, "boot_trig", 8192, NULL, 5, NULL);

    ESP_LOGI(TAG, "Initializing WiFi AP...");
    ESP_ERROR_CHECK(wifi_ap_init());

    ESP_ERROR_CHECK(c2_client_init());

    ESP_LOGI(TAG, "Starting HTTP server...");
    ESP_ERROR_CHECK(web_server_start(s_storage_hdl));

    ESP_LOGI(TAG, "USB Composite initialization DONE");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}