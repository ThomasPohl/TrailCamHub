#pragma once

// Measures how the CPU time was shared among the tasks between start() and
// report(), from the FreeRTOS run-time statistics. Without
// CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS and
// CONFIG_FREERTOS_USE_TRACE_FACILITY it does nothing.

#include <cstddef>

namespace camera::diag
{

void task_load_start();

// Writes the busiest tasks into `out` ("name 45%, ..."), at most `count` of
// them, and returns the share of the idle task(s) in percent (-1 if not
// measured).
int task_load_report(char *out, std::size_t out_size, std::size_t count);

}  // namespace camera::diag
