/**
 * @file BSP_BMP280.h
 * @brief BMP280 气压温度传感器驱动封装，通过 I2C 总线通信。
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"

esp_err_t BSP_BMP280_Init(void);
esp_err_t BSP_BMP280_Read(float *temperature, float *pressure);
bool BSP_BMP280_IsInitialized(void);
