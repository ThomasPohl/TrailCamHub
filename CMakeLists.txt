cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(trailcamhub_ble_poc)

# ESP-IDF's NimBLE host (ble_hs_pvcy.c) triggers a false-positive
# -Wmaybe-uninitialized when optimizing for size (release profile). Keep it
# a warning for the Bluetooth component only.
idf_component_get_property(bt_lib bt COMPONENT_LIB)
target_compile_options(${bt_lib} PRIVATE -Wno-error=maybe-uninitialized)
