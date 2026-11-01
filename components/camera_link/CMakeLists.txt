idf_component_register(SRCS "camera_protocol.cpp" "camera_prelogin.cpp"
                            "camera_udp_packets.cpp" "camera_udp_transport.cpp"
                            "pppp_session.cpp"
                            "artemis_aes128.cpp" "artemis_crypto.cpp"
                            "artemis_json.cpp" "artemis_message.cpp"
                            "artemis_login.cpp" "artemis_reassembler.cpp"
                            "camera_clock.cpp"
                            "media_list.cpp" "media_transfer.cpp"
                            "bulk_reassembler.cpp" "bulk_stream.cpp"
                            "device_info.cpp" "frame_reorder.cpp"
                            "media_catalog.cpp" "media_store.cpp"
                            "playback_decoder.cpp" "playback_record.cpp"
                            "playback_stream.cpp" "sd_writer.cpp"
                            "task_load.cpp"
                       # "." exposes the pure protocol/catalog headers to the
                       # uplink component.
                       INCLUDE_DIRS "include" "."
                       REQUIRES esp_netif
                       PRIV_REQUIRES esp_app_format esp_ringbuf esp_timer esp_wifi lwip mbedtls
                                     sd_storage time_sync)
