# Optional SSD1306 OLED (#224); empty unless CONFIG_HUB_OLED is set. The
# screen texts and the font are the dongle display's, shared by path.
set(srcs)
if(CONFIG_HUB_OLED)
    list(APPEND srcs "oled_ui.cpp" "ssd1306.cpp" "oled_renderer.cpp"
                     "${CMAKE_CURRENT_LIST_DIR}/../dongle_ui/status_screen.cpp"
                     "${CMAKE_CURRENT_LIST_DIR}/../dongle_ui/font5x7.cpp")
endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "." "../dongle_ui"
                       REQUIRES dongle_ui
                       PRIV_REQUIRES esp_driver_i2c esp_timer hub_battery)
