idf_component_register(SRCS "uplink.cpp" "uplink_protocol.cpp"
                            "backend.cpp" "backend_http.cpp"
                            "backend_checkin.cpp" "backend_commands.cpp"
                            "video_request.cpp" "http_util.cpp"
                            "push_protocol.cpp" "destinations.cpp"
                            "sync_cameras.cpp" "delete_request.cpp"
                            "mp4_body.cpp"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "."
                       REQUIRES hub_config
                       PRIV_REQUIRES camera_link camera_wifi diagnostics esp_app_format
                                     esp_http_client esp_timer hub_battery
                                     mbedtls modem_uplink mp4_mux ota_update
                                     schedule
                                     schedule_state time_sync)
