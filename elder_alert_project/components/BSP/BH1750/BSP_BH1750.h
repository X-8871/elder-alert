/**
 * @file BSP_BH1750.h
 * @brief BH1750 光照度传感器驱动封装，通过 I2C 总线通信，连续高分辨率模式。
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t BSP_BH1750_Init(void);
esp_err_t BSP_BH1750_Read(uint16_t *lux);
bool BSP_BH1750_IsInitialized(void);
