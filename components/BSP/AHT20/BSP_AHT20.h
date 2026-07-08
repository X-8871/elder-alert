/**
 * @file BSP_AHT20.h
 * @brief AHT20 温湿度传感器驱动封装——通过共享 I2C 总线通信。
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"

/**
 * @brief 初始化 AHT20 传感器。
 * @return ESP_OK 成功，其他值表示失败
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
