/**
 * @file BSP_I2C.h
 * @brief 共享 I2C 总线管理——为 AHT20/BH1750/OLED 等设备提供统一的总线初始化接口。
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_err.h"

/* ---- 初始化 ---- */

/**
 * @brief 初始化共享 I2C 总线（整个系统只调用一次）。
 *
 * @param port         I2C 控制器编号，填 I2C_NUM_0 或 I2C_NUM_1
 * @param sda_io_num   SDA 数据线对应的 GPIO 引脚号
 * @param scl_io_num   SCL 时钟线对应的 GPIO 引脚号
 * @param clk_speed_hz 时钟频率（Hz），通常 100000（100kHz 标准模式）
 *
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数不合法；ESP_ERR_INVALID_STATE 已用不同参数初始化过
 */
esp_err_t BSP_I2C_Init(i2c_port_t port, gpio_num_t sda_io_num, gpio_num_t scl_io_num, uint32_t clk_speed_hz);

/* ---- 状态查询 ---- */

bool BSP_I2C_IsInitialized(void);

/**
 * @brief 获取当前 I2C 控制器编号。
 */
i2c_port_t BSP_I2C_GetPort(void);

/** 获取 SDA 数据线对应的 GPIO */
gpio_num_t BSP_I2C_GetSDA(void);

/** 获取 SCL 时钟线对应的 GPIO */
gpio_num_t BSP_I2C_GetSCL(void);

/** 获取当前 I2C 时钟频率（Hz） */
uint32_t BSP_I2C_GetClockSpeed(void);
