# Pure schedule logic (plans, due times, read requests); no ESP-IDF
# dependencies, also built by the host tests.
idf_component_register(SRCS "schedule_plan.cpp" "sun.cpp" "energy.cpp"
                            "scheduler_step.cpp"
                       INCLUDE_DIRS ".")
