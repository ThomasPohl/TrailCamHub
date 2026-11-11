# Camera mock for bench tests (#122): an ESP32-WROOM that stands in for a
# TC100 camera. A separate ESP-IDF project; see README.md.
#
#   idf.py -C tools/camera_mock set-target esp32
#   idf.py -C tools/camera_mock build flash monitor
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(trailcamhub_camera_mock)

# ESP-IDF's NimBLE host can trigger a false-positive -Wmaybe-uninitialized
# (see the firmware's CMakeLists.txt).
idf_component_get_property(bt_lib bt COMPONENT_LIB)
target_compile_options(${bt_lib} PRIVATE -Wno-error=maybe-uninitialized)
