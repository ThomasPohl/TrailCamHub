# Serial console; the diagnostic commands only with
# CONFIG_HUB_CONSOLE_DIAGNOSTICS.
set(srcs "hub_console.cpp" "command_parser.cpp")
if(CONFIG_HUB_CONSOLE_DIAGNOSTICS)
    list(APPEND srcs "diagnostics.cpp")
endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_driver_uart esp_driver_usb_serial_jtag
                                     esp_rom esp_timer fatfs heap hub_config
                                     mbedtls sd_log sd_storage
                                     session_controller vfs)
