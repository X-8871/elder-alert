/**
 * @file BSP_BH1750.h
 * @brief BH1750 光照度传感器驱动封装——通过共享 I2C 总线通信。
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t BSP_BH1750_Init(void);

/**
 * @brief 读取一次光照度。
 * @param lux 输出：光照值（lux），范围通常 1 ~ 65535
 */
esp_err_t BSP_BH1750_Read(uint16_t *lux);

bool BSP_BH1750_IsInitialized(void);
