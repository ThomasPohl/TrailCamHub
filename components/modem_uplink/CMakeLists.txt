# Backend uplink over the cellular modem (USB host, PPP). Without
# CONFIG_MODEM_UPLINK only the stubs are built. The modem driver comes from
# the component registry (idf_component.yml, ESP32-S3 only).
set(srcs "modem_uplink.cpp")
set(priv_requires esp_event esp_netif esp_timer)
if(CONFIG_MODEM_UPLINK)
    list(APPEND srcs "modem_ids.c")
    list(APPEND priv_requires esp_driver_gpio espressif__iot_usbh_modem
                              espressif__iot_usbh_cdc espressif__modem_at)
endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES ${priv_requires})
