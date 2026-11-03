#pragma once

#include "esp_err.h"

/**
 * @brief Inizializza NVS e avvia SoftAP.
 *        SSID: DuckRelay, password: password123, IP: 192.168.4.1
 */
esp_err_t wifi_ap_init(void);