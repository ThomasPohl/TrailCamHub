# Sets the clock: by SNTP (CONFIG_TIME_SYNC_SNTP) and from the other sources
# that time_sync_set() takes.
idf_component_register(SRCS "time_sync.cpp" "time_policy.cpp"
                       INCLUDE_DIRS "include" "."
                       PRIV_REQUIRES esp_app_format esp_event esp_netif lwip)
