idf_component_register(SRCS "main.cpp"
                       PRIV_REQUIRES camera_ble camera_link coprocessor_update
                                     diagnostics dongle_ui esp_event esp_netif
                                     ethernet_uplink hub_console nvs_flash
                                     sd_log sd_storage session_controller
                                     time_sync web_ui)
