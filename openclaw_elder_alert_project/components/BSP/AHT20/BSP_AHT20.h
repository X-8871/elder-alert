#pragma once

#include <stdbool.h>
#include "esp_err.h"

esp_err_t BSP_AHT20_Init(void);
esp_err_t BSP_AHT20_Read(float *temperature, float *humidity);
bool BSP_AHT20_IsInitialized(void);
