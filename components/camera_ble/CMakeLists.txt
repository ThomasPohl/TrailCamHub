idf_component_register(SRCS "camera_ble.cpp" "camera_ble_discovery.cpp"
                            "camera_ble_protocol.cpp" "ble_advertising.cpp"
                            "ble_wifi_response.cpp" "gatt_profile.cpp"
                            "hex_payload.cpp"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "."
                       PRIV_REQUIRES bt json)
