#pragma once
#include "hal/gpio_types.h"
#include "esp_err.h"

esp_err_t dht22_read(gpio_num_t pin, float *temperature, float *humidity);