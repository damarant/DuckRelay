#pragma once

#include <stdbool.h>
#include "wifi_ap.h"
#include "esp_err.h"

/**
 * @brief Inizializza il parser (reset delay default, layout IT).
 */
esp_err_t payload_parser_init(void);

/**
 * @brief Imposta il layout tastiera per le prossime esecuzioni.
 *
 * @param italian true = layout IT (default), false = layout US.
 */
void payload_set_layout(bool italian);

/**
 * @brief Ritorna il layout corrente (true = IT, false = US).
 */
bool payload_get_layout(void);

/**
 * @brief Esegue uno script DuckyScript da stringa in memoria.
 * Richiede che il device USB sia montato (tud_mounted()).
 *
 * @param script Script multi-linea, terminato da '\0'.
 * @return ESP_OK, ESP_ERR_INVALID_ARG, ESP_ERR_INVALID_STATE.
 */
esp_err_t payload_execute_string(const char *script);

/**
 * @brief Esegue uno script DuckyScript da file (es. "/usb/payload.txt").
 * Implementazione prevista in Fase 2.
 */
esp_err_t payload_execute_file(const char *path);