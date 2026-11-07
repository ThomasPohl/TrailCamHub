# Requests the scheduled cycles and puts the hub to sleep in between.
idf_component_register(SRCS "scheduler.cpp"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES camera_link esp_hw_support esp_wifi
                                     modem_uplink schedule schedule_state
                                     sd_log
                                     session_controller)
