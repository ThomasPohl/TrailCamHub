idf_component_register(SRCS "main.cpp"
                       PRIV_REQUIRES bt nvs_flash camera_ble camera_link
                                     camera_wifi diagnostics sd_storage)
