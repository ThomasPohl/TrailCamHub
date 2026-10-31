# Renders the T-Dongle status screens as PNG files for the documentation,
# with the same code as the firmware:
#
#   cmake -S tools/dongle_screens -B build-screens
#   cmake --build build-screens
#   build-screens/dongle_screens docs/images/dongle
cmake_minimum_required(VERSION 3.16)
project(dongle_screens LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(ui_dir ${CMAKE_CURRENT_SOURCE_DIR}/../../components/dongle_ui)
add_executable(dongle_screens
    main.cpp
    png_writer.cpp
    ${ui_dir}/status_screen.cpp
    ${ui_dir}/screen_renderer.cpp
    ${ui_dir}/font5x7.cpp)
target_include_directories(dongle_screens PRIVATE ${ui_dir})
target_compile_options(dongle_screens PRIVATE -Wall -Wextra -Werror)
