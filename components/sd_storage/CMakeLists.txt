idf_component_register(SRCS "sd_card.cpp"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_driver_sdmmc esp_driver_sdspi esp_driver_spi
                                     fatfs sdmmc)
