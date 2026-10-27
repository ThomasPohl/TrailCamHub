idf_component_register(SRCS "main.cpp"
                       PRIV_REQUIRES bt diagnostics nvs_flash sd_storage
                                     session_controller)
