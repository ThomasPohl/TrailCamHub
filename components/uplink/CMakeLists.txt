idf_component_register(SRCS "uplink.cpp" "uplink_protocol.cpp"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "."
                       REQUIRES hub_config
                       PRIV_REQUIRES camera_link esp_app_format
                                     esp_http_client mbedtls time_sync)
