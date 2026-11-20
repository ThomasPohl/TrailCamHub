#pragma once
#include <stdint.h>
// Host: sleeps, in real time.
extern "C" void host_task_delay_ms(uint32_t ms);
#define pdMS_TO_TICKS(ms) (ms)
#define vTaskDelay(ticks) host_task_delay_ms(ticks)
