# Firmware updates over the backend check-in: ota_policy.cpp is pure logic
# (also built by the host tests), ota_update.cpp downloads and flashes.
idf_component_register(SRCS "ota_policy.cpp" "ota_update.cpp"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "."
                       PRIV_REQUIRES app_update esp_app_format
                                     esp_http_client esp_partition esp_timer
                                     mbedtls sd_log)
