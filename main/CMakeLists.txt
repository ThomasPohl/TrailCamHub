idf_component_register(SRCS "main.c" "camera_ble.c"
                       "camera_ble_discovery.c" "camera_ble_profile.c"
                       "camera_ble_protocol.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES bt json nvs_flash)