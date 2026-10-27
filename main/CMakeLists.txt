idf_component_register(SRCS "main.cpp" "camera_ble.cpp"
                       "camera_ble_discovery.cpp" "camera_ble_profile.cpp"
                       "camera_ble_protocol.cpp" "camera_wifi.cpp"
                       "camera_protocol.cpp" "camera_udp_transport.cpp"
                       "camera_udp_packets.cpp" "sd_card.cpp"
                       "system_diagnostics.cpp"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES bt esp_driver_sdmmc esp_event esp_netif
                                     esp_timer esp_wifi espcoredump fatfs
                                     json lwip mbedtls nvs_flash sdmmc)
