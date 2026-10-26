idf_component_register(SRCS "main.cpp" "camera_ble.cpp"
                       "camera_ble_discovery.cpp" "camera_ble_profile.cpp"
                       "camera_ble_protocol.cpp" "camera_wifi.cpp"
                       "camera_protocol.cpp" "camera_udp_transport.cpp"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES bt esp_event esp_netif esp_timer esp_wifi
                                     json lwip mbedtls nvs_flash)