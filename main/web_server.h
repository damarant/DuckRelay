#pragma once

#include "esp_err.h"
#include "tinyusb_msc.h"

esp_err_t web_server_start(tinyusb_msc_storage_handle_t storage_hdl);