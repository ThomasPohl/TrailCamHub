# Web UI over the dongle's own access point, Ethernet or the home Wi-Fi;
# empty unless enabled. mDNS (trailcamhub.local) comes from the component
# registry (idf_component.yml).
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
                       PRIV_REQUIRES camera_link esp_event esp_http_server esp_netif
                                     esp_timer esp_wifi fatfs lwip nvs_flash
                                     schedule schedule_state sd_log sd_storage
                                     mp4_mux time_sync espressif__mdns)
