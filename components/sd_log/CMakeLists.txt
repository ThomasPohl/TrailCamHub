# Log copy on the SD card; empty unless enabled.
set(srcs)
if(CONFIG_SD_LOG_ENABLE)
    list(APPEND srcs "sd_log.cpp")
endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_ringbuf sd_storage)
