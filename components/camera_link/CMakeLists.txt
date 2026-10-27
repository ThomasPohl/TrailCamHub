idf_component_register(SRCS "camera_protocol.cpp" "camera_prelogin.cpp"
                            "camera_udp_packets.cpp" "camera_udp_transport.cpp"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "."
                       REQUIRES esp_netif
                       PRIV_REQUIRES esp_timer lwip mbedtls)
