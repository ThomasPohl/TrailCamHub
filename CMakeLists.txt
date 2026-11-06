cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# The CI sets the version of a release build to its tag (docs/ota.md);
# otherwise ESP-IDF takes `git describe` (a commit hash in the CI).
if(DEFINED ENV{TRAILCAMHUB_VERSION} AND NOT "$ENV{TRAILCAMHUB_VERSION}" STREQUAL "")
    set(PROJECT_VER "$ENV{TRAILCAMHUB_VERSION}")
endif()

project(trailcamhub_ble_poc)

# ESP-IDF's NimBLE host (ble_hs_pvcy.c) triggers a false-positive
# -Wmaybe-uninitialized when optimizing for size (release profile). Keep it
# a warning for the Bluetooth component only.
idf_component_get_property(bt_lib bt COMPONENT_LIB)
target_compile_options(${bt_lib} PRIVATE -Wno-error=maybe-uninitialized)
