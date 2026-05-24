/**
 * @file BSP_AHT20.h
 * @brief AHT20 温湿度传感器驱动封装——通过共享 I2C 总线通信。
 *
 * 【学弟必读】
 * AHT20 是一个 I2C 接口的数字温湿度传感器，可以同时输出温度(°C)和相对湿度(%)。
 * - 通信地址：0x38（AHT_I2C_ADDRESS_GND，即 ADDR 引脚接 GND）
 * - 依赖共享 I2C 总线（components/BSP/I2C/），使用前必须先初始化 I2C。
 */

#pragma once

#include <stdbool.h>   /* bool 类型 */
#include "esp_err.h"   /* ESP-IDF 错误码 */

/**
 * @brief 初始化 AHT20 传感器。
 *        会自动从共享 I2C 总线获取端口和引脚配置。
 * @return ESP_OK 成功，其他值表示失败（比如 I2C 未初始化或传感器未连接）
 */
esp_err_t BSP_AHT20_Init(void);

/**
 * @brief 读取一次温湿度数据。
 * @param temperature 输出：温度值（°C）
 * @param humidity    输出：相对湿度（%RH）
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 传感器未初始化；
 *         ESP_ERR_INVALID_ARG 参数为空指针
 */
esp_err_t BSP_AHT20_Read(float *temperature, float *humidity);

/** 查询传感器是否已完成初始化 */
bool BSP_AHT20_IsInitialized(void);
