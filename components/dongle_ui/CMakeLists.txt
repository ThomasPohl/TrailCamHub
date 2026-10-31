# Display and button of the LilyGO T-Dongle-S3/-C5; empty on other boards.
set(srcs)
if(CONFIG_BOARD_TDONGLE)
    list(APPEND srcs "dongle_ui.cpp" "st7735_display.cpp" "font5x7.cpp"
                     "status_screen.cpp" "button_detector.cpp"
                     "screen_renderer.cpp" "status_led.cpp")
endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_driver_gpio esp_driver_spi esp_lcd
                                     esp_timer sd_storage)
