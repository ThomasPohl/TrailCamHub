#pragma once
// Host: a mutex semaphore is a recursive mutex (#446).
#include "freertos/FreeRTOS.h"

struct HostSemaphore;
typedef HostSemaphore *SemaphoreHandle_t;
typedef uint32_t TickType_t;
#define portMAX_DELAY 0xffffffffu
#define pdTRUE 1
#define pdFALSE 0
extern "C" {
SemaphoreHandle_t xSemaphoreCreateMutex(void);
SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void);
int xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t ticks);
int xSemaphoreGive(SemaphoreHandle_t semaphore);
int xSemaphoreTakeRecursive(SemaphoreHandle_t semaphore, TickType_t ticks);
int xSemaphoreGiveRecursive(SemaphoreHandle_t semaphore);
}
