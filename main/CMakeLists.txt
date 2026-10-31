idf_component_register(SRCS "main.cpp"
                       PRIV_REQUIRES camera_link diagnostics dongle_ui esp_event
                                     esp_netif hub_console nvs_flash sd_log
                                     sd_storage session_controller)
