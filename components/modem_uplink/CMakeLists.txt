# Backend uplink over the cellular modem (USB host, PPP). Without
# CONFIG_MODEM_UPLINK only the stubs are built.
set(srcs "modem_uplink.cpp")
set(priv_requires esp_event esp_netif esp_timer)
if(CONFIG_MODEM_UPLINK)
    list(APPEND srcs "modem_ids.c")
    list(APPEND priv_requires esp_driver_gpio)
endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES ${priv_requires})
