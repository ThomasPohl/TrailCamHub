# The hub's own battery: battery_level.cpp is pure logic (also built by the
# host tests), hub_battery.cpp reads the MAX17048 fuel gauge over I2C.
idf_component_register(SRCS "battery_level.cpp" "hub_battery.cpp"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_driver_i2c)
