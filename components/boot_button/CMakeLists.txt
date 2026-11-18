# The BOOT button (GPIO0) as the "fetch now" button on boards without a
# button of their own (#224). Empty unless CONFIG_HUB_BOOT_BUTTON is set. The
# debouncing is the dongles' (host-tested in test/host/test_button_detector.cpp).
set(srcs)
if(CONFIG_HUB_BOOT_BUTTON)
    list(APPEND srcs "boot_button.cpp"
                     "${CMAKE_CURRENT_LIST_DIR}/../dongle_ui/button_detector.cpp")
endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "." "../dongle_ui"
                       PRIV_REQUIRES esp_driver_gpio esp_timer)
