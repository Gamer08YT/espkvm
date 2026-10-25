#pragma once
#include <stddef.h>
#include <string.h>
static inline void esp_fill_random(void *buf, size_t len)
{
    memset(buf, 0x5a, len);
}
