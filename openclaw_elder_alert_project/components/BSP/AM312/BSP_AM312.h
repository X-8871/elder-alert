#pragma once

#include <stdbool.h>

#include "driver/gpio.h"
#include "esp_err.h"

esp_err_t BSP_AM312_Init(gpio_num_t signal_gpio, bool active_high);
esp_err_t BSP_AM312_IsMotionDetected(bool *detected);
esp_err_t BSP_AM312_GetRawLevel(int *level);
bool BSP_AM312_IsInitialized(void);
