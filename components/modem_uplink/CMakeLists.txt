# Backend uplink over the cellular modem (USB host, PPP). Without
# CONFIG_MODEM_UPLINK only the stubs are built. The modem driver comes from
# the component registry (idf_component.yml) for the ESP32-S3 only; the
# requirements depend on the target, not on Kconfig, because ESP-IDF
# resolves them before the configuration is known.
set(srcs "modem_uplink.cpp")
set(priv_requires esp_event esp_netif esp_timer)
if(IDF_TARGET STREQUAL "esp32s3")
    list(APPEND priv_requires esp_driver_gpio espressif__iot_usbh_modem
                              espressif__iot_usbh_cdc espressif__modem_at)
endif()
if(CONFIG_MODEM_UPLINK)
    list(APPEND srcs "modem_ids.c")
endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES ${priv_requires} hub_config)
