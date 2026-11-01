idf_component_register(SRCS "camera_wifi.cpp" "disconnect_policy.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_netif
                       PRIV_REQUIRES esp_event esp_wifi)
