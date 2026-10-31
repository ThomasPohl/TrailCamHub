# Web UI over the dongle's own access point; empty unless enabled.
set(srcs)
set(embed)
if(CONFIG_WEB_UI_ENABLE)
    list(APPEND srcs "web_ui.cpp" "web_paths.cpp")
    list(APPEND embed "www/index.html" "www/style.css" "www/app.js")
endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "."
                       EMBED_TXTFILES ${embed}
                       PRIV_REQUIRES esp_event esp_http_server esp_netif
                                     esp_timer esp_wifi fatfs lwip nvs_flash
                                     sd_log sd_storage)
