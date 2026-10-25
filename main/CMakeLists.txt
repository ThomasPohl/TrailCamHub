idf_component_register(SRCS "main.c" "camera_ble.c"
                       "camera_ble_discovery.c" "camera_ble_profile.c"
                       "camera_ble_protocol.c" "camera_wifi.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES bt esp_event esp_http_client esp_netif
                                     esp_wifi json nvs_flash)