# Only targets with an on-chip Ethernet MAC (ESP32-P4) build the driver.
set(srcs)
if(CONFIG_ETH_UPLINK_ENABLE)
    list(APPEND srcs "ethernet_uplink.cpp")
endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_eth esp_event esp_netif)
