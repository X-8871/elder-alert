/**
 * @file BSP_I2C.h
 * @brief 共享 I2C 总线管理，为 AHT20/BMP280/BH1750/OLED 等设备提供统一的总线初始化和配置查询。
 *
 * 全局只初始化一次，重复调用相同配置不会报错；不同配置会被拒绝。
 * 底层依赖 i2cdev 组件。
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_err.h"

esp_err_t BSP_I2C_Init(i2c_port_t port, gpio_num_t sda_io_num, gpio_num_t scl_io_num, uint32_t clk_speed_hz);
bool BSP_I2C_IsInitialized(void);
i2c_port_t BSP_I2C_GetPort(void);
gpio_num_t BSP_I2C_GetSDA(void);
gpio_num_t BSP_I2C_GetSCL(void);
uint32_t BSP_I2C_GetClockSpeed(void);
