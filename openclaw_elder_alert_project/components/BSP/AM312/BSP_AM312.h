/**
 * @file BSP_AM312.h
 * @brief AM312 人体红外传感器驱动封装，纯 GPIO 输入检测。
 *
 * 支持高/低电平有效的极性配置，提供原始电平和已解释的活动检测结果。
 */

#pragma once

#include <stdbool.h>

#include "driver/gpio.h"
#include "esp_err.h"

esp_err_t BSP_AM312_Init(gpio_num_t signal_gpio, bool active_high);

/** 读取已按极性解释的人体活动检测结果。 */
esp_err_t BSP_AM312_IsMotionDetected(bool *detected);

/** 读取 GPIO 原始电平值（0 或 1）。 */
esp_err_t BSP_AM312_GetRawLevel(int *level);

bool BSP_AM312_IsInitialized(void);
