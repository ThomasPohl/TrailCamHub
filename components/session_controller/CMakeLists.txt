idf_component_register(SRCS "session_controller.cpp" "session_fsm.cpp"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "."
                       PRIV_REQUIRES camera_ble camera_link camera_wifi
                                     diagnostics esp_netif esp_timer)
