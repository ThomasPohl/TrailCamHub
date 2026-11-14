idf_component_register(SRCS "system_diagnostics.cpp" "cycle_trace.cpp"
                            "cycle_record.cpp"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "."
                       PRIV_REQUIRES espcoredump esp_timer hub_battery sd_log)
