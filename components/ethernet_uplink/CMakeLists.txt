# Wired Ethernet: the on-chip MAC (ESP32-P4) or a W5500 on SPI (ESP32-S3-ETH).
# Without CONFIG_ETH_UPLINK_ENABLE nothing is built. The requirements cannot
# depend on Kconfig (ESP-IDF resolves them first), so the SPI and GPIO
# drivers are always listed.
set(srcs)
if(CONFIG_ETH_UPLINK_ENABLE)
    list(APPEND srcs "ethernet_uplink.cpp")
endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_eth esp_event esp_netif
                                     esp_driver_gpio esp_driver_spi esp_hw_support)
