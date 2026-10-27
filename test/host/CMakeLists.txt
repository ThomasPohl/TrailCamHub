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

add_executable(host_tests
    test_main.cpp
    test_ble_advertising.cpp
    test_ble_wifi_response.cpp
    test_hex_payload.cpp
    test_pppp_session.cpp
    test_prelogin.cpp
    test_session_fsm.cpp
    test_udp_packets.cpp
    ${ble_dir}/ble_advertising.cpp
    ${ble_dir}/ble_wifi_response.cpp
    ${ble_dir}/hex_payload.cpp
    ${link_dir}/camera_prelogin.cpp
    ${link_dir}/camera_udp_packets.cpp
    ${link_dir}/pppp_session.cpp
    ${session_dir}/session_fsm.cpp)
target_include_directories(host_tests PRIVATE ${ble_dir} ${link_dir}
                                              ${session_dir})
target_include_directories(host_tests SYSTEM PRIVATE ${doctest_SOURCE_DIR})
target_compile_options(host_tests PRIVATE -Wall -Wextra -Werror)

enable_testing()
add_test(NAME host_tests COMMAND host_tests)
