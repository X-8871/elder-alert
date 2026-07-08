/**
 * @file BSP_BH1750.h
 * @brief BH1750 光照度传感器驱动封装——通过共享 I2C 总线通信。
 *
 * 【学弟必读】
 * BH1750 是一个 I2C 接口的数字环境光传感器，输出单位为 lux（勒克斯）。
 * - 通信地址：0x23（BH1750_ADDR_LO，即 ADDR 引脚接低电平）
 * - 工作模式：连续高分辨率模式（Continuous High Resolution），每秒约能读 1-2 次。
 * - 依赖共享 I2C 总线。
 * - 本项目用它来判断环境是否"低光"（比如晚上关灯了），用于休息上下文判断。
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
