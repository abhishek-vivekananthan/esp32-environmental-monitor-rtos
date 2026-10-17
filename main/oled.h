#pragma once
#include "esp_err.h"
#include <stdint.h>

#define OLED_ADDR    0x3C
#define OLED_WIDTH   128
#define OLED_HEIGHT  64

esp_err_t oled_init(void);
esp_err_t oled_clear(void);
esp_err_t oled_print(uint8_t row, const char *text);