idf_component_register(SRCS "sd_card.cpp"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_driver_sdmmc fatfs sdmmc)
