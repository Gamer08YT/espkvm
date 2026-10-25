/* Host stand-ins for the bits of ESP-IDF the settings store uses. */
#pragma once
#include <stdio.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_INVALID_SIZE 0x104
#define ESP_ERR_NOT_FOUND 0x105
#define ESP_ERR_NOT_ALLOWED 0x10C
static inline const char *esp_err_to_name(esp_err_t e)
{
    (void)e;
    return "error";
}
