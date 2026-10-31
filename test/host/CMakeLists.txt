# Host unit tests for the hardware-independent firmware logic.
#
#   cmake -S test/host -B build-host && cmake --build build-host
#   ctest --test-dir build-host --output-on-failure
cmake_minimum_required(VERSION 3.16)
project(trailcamhub_host_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# doctest is header-only; only its header is used (SOURCE_SUBDIR skips its
# CMake project).
include(FetchContent)
FetchContent_Declare(
    doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG v2.5.3
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR do-not-add)
FetchContent_MakeAvailable(doctest)

set(repo_root ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(ble_dir ${repo_root}/components/camera_ble)
set(link_dir ${repo_root}/components/camera_link)
set(session_dir ${repo_root}/components/session_controller)
set(ui_dir ${repo_root}/components/dongle_ui)
set(config_dir ${repo_root}/components/hub_config)
set(console_dir ${repo_root}/components/hub_console)
set(uplink_dir ${repo_root}/components/uplink)

add_executable(host_tests
    test_main.cpp
    test_ble_advertising.cpp
    test_ble_wifi_response.cpp
    test_button_detector.cpp
    test_gatt_profile.cpp
    test_hex_payload.cpp
    test_artemis_crypto.cpp
    test_artemis_login.cpp
    test_artemis_message.cpp
    test_artemis_reassembler.cpp
    test_media_list.cpp
    test_media_transfer.cpp
    test_bulk_reassembler.cpp
    test_bulk_stream.cpp
    test_device_info.cpp
    test_media_catalog.cpp
    test_media_store.cpp
    test_pppp_session.cpp
    test_playback.cpp
    test_playback_decoder.cpp
    test_prelogin.cpp
    test_session_fsm.cpp
    test_status_screen.cpp
    test_screen_renderer.cpp
    test_udp_packets.cpp
    test_hub_console.cpp
    test_uplink_protocol.cpp
    ${ble_dir}/ble_advertising.cpp
    ${ble_dir}/ble_wifi_response.cpp
    ${ble_dir}/gatt_profile.cpp
    ${ble_dir}/hex_payload.cpp
    ${link_dir}/artemis_aes128.cpp
    ${link_dir}/artemis_crypto.cpp
    ${link_dir}/artemis_json.cpp
    ${link_dir}/artemis_login.cpp
    ${link_dir}/artemis_message.cpp
    ${link_dir}/artemis_reassembler.cpp
    ${link_dir}/media_list.cpp
    ${link_dir}/media_transfer.cpp
    ${link_dir}/bulk_reassembler.cpp
    ${link_dir}/bulk_stream.cpp
    ${link_dir}/frame_reorder.cpp
    ${link_dir}/playback_decoder.cpp
    ${link_dir}/playback_record.cpp
    ${link_dir}/playback_stream.cpp
    ${link_dir}/device_info.cpp
    ${link_dir}/media_catalog.cpp
    ${link_dir}/camera_prelogin.cpp
    ${link_dir}/camera_udp_packets.cpp
    ${link_dir}/pppp_session.cpp
    ${session_dir}/session_fsm.cpp
    ${ui_dir}/button_detector.cpp
    ${ui_dir}/status_screen.cpp
    ${ui_dir}/screen_renderer.cpp
    ${ui_dir}/font5x7.cpp
    ${config_dir}/hub_config_rules.cpp
    ${console_dir}/command_parser.cpp
    ${uplink_dir}/uplink_protocol.cpp)
target_include_directories(host_tests PRIVATE ${ble_dir} ${link_dir}
                                              ${session_dir} ${ui_dir}
                                              ${config_dir}/include
                                              ${console_dir} ${uplink_dir})
target_include_directories(host_tests SYSTEM PRIVATE ${doctest_SOURCE_DIR})
target_compile_options(host_tests PRIVATE -Wall -Wextra -Werror)

enable_testing()
add_test(NAME host_tests COMMAND host_tests)
