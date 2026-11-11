# Only the low-level primitives come from the hub (AES-128, Base64, ARTEMIS
# request parsing); everything that answers the hub is in ../core (#122).
set(core_dir ${CMAKE_CURRENT_LIST_DIR}/../core)
set(link_dir ${CMAKE_CURRENT_LIST_DIR}/../../../components/camera_link)

idf_component_register(SRCS "main.cpp" "mock_ap.cpp" "mock_ble.c"
                            "mock_console.cpp" "mock_udp.cpp"
                            "${core_dir}/ble_frames.cpp"
                            "${core_dir}/capture_fixtures.cpp"
                            "${core_dir}/mock_camera.cpp"
                            "${core_dir}/mock_wire.cpp"
                            "${link_dir}/artemis_aes128.cpp"
                            "${link_dir}/artemis_crypto.cpp"
                            "${link_dir}/artemis_message.cpp"
                       INCLUDE_DIRS "." "${core_dir}" "${link_dir}"
                       PRIV_REQUIRES bt console esp_event esp_netif esp_timer
                                     esp_wifi lwip nvs_flash)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror
                                                -Wno-missing-field-initializers)
