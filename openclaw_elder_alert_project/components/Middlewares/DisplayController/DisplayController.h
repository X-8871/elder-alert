/**
 * @file DisplayController.h
 * @brief OLED 显示控制器，将应用状态、传感器数据、风险等级格式化输出到 128×64 SSD1306 屏幕。
 *
 * 4 行显示布局：
 *   行0: 应用状态 + 传感器健康状态
 *   行1: 温度 + 湿度
 *   行2: 气压 + 光照度
 *   行3: 风险等级 + WiFi 状态
 */

#pragma once

#include <stdbool.h>

#include "AppController.h"
#include "RiskEngine.h"
#include "SensorHub.h"
#include "esp_err.h"

/** 初始化 OLED 显示屏，不可用时自动禁用显示功能。 */
esp_err_t DisplayController_Init(void);

/** 将最新状态刷新到 OLED 屏幕。 */
esp_err_t DisplayController_Update(app_state_t app_state,
                                   const sensor_hub_data_t *sensor_data,
                                   const risk_result_t *risk_result);

/** 查询 OLED 是否可用。 */
bool DisplayController_IsEnabled(void);
