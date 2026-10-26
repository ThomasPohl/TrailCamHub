idf_component_register(SRCS "main.c" "camera_ble.c"
                       "camera_ble_discovery.c" "camera_ble_profile.c"
                       "camera_ble_protocol.c" "camera_wifi.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES bt esp_event esp_netif esp_timer esp_wifi
                                     json lwip nvs_flash)