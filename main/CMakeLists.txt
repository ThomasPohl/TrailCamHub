idf_component_register(SRCS "main.c" "camera_ble.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES bt nvs_flash)