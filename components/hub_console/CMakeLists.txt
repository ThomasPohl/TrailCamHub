idf_component_register(SRCS "hub_console.cpp" "command_parser.cpp"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_driver_uart esp_driver_usb_serial_jtag
                                     vfs hub_config session_controller)
