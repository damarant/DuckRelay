idf_component_register(
    SRCS "tusb_composite_main.c"
         "payload_parser.c"
         "wifi_ap.c"
         "web_server.c"
         "c2_client.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_netif esp_event nvs_flash esp_http_server espressif__esp_tinyusb
)