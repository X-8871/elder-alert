/**
 * @file BSP_AHT20.h
 * @brief AHT20 温湿度传感器驱动封装，通过 I2C 总线通信。
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"

esp_err_t BSP_AHT20_Init(void);
esp_err_t BSP_AHT20_Read(float *temperature, float *humidity);
bool BSP_AHT20_IsInitialized(void);
