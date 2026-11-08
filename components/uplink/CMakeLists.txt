idf_component_register(SRCS "uplink.cpp" "uplink_protocol.cpp"
                            "video_request.cpp" "http_util.cpp"
                            "push_protocol.cpp" "destinations.cpp"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "."
                       REQUIRES hub_config
                       PRIV_REQUIRES camera_link camera_wifi esp_app_format
                                     esp_http_client esp_timer hub_battery
                                     mbedtls modem_uplink mp4_mux ota_update
                                     schedule
                                     schedule_state time_sync)
