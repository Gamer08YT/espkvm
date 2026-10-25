#pragma once
#include <stdint.h>
typedef int *SemaphoreHandle_t;
static int s_stub_sem;
static inline SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    return &s_stub_sem;
}
static inline int xSemaphoreTake(SemaphoreHandle_t h, uint32_t t)
{
    (void)h;
    (void)t;
    return 1;
}
static inline int xSemaphoreGive(SemaphoreHandle_t h)
{
    (void)h;
    return 1;
}
