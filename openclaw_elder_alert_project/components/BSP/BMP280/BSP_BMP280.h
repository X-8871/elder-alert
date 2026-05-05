#pragma once

#include <stdbool.h>
#include "esp_err.h"

esp_err_t BSP_BMP280_Init(void);
esp_err_t BSP_BMP280_Read(float *temperature, float *pressure);
bool BSP_BMP280_IsInitialized(void);
