#pragma once
#include "esp_err.h"
#define ESP_RETURN_ON_FALSE(a, err, tag, ...) \
    do {                                      \
        (void)(tag);                          \
        if (!(a)) {                           \
            return (err);                     \
        }                                     \
    } while (0)
