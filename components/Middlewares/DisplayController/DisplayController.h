/**
 * @file DisplayController.h
 * @brief 显示控制器，将应用状态、风险原因和按键提示渲染到 ST7735 TFT 屏幕。
 *
 * 当前界面基于 LVGL，按项目四态状态机展示：
 *   - NORMAL  绿色主背景
 *   - REMIND  橙色主背景
 *   - ALARM   红色主背景
 *   - SOS     深红主背景
 *
 * 联网异常不会进入第五状态，而是在任意主状态上叠加灰色离线提示。
 */

#pragma once

#include <stdbool.h>

#include "AppController.h"
#include "RiskEngine.h"
#include "SensorHub.h"
#include "esp_err.h"

/** 初始化 TFT + LVGL 显示链路，不可用时自动禁用显示功能。 */
esp_err_t DisplayController_Init(void);

/** 将最新状态刷新到 TFT 界面。 */
esp_err_t DisplayController_Update(app_state_t app_state,
                                   const sensor_hub_data_t *sensor_data,
                                   const risk_result_t *risk_result);

/** 推进 LVGL 的 tick 和重绘服务，建议每 100ms 调一次。 */
esp_err_t DisplayController_Service(uint32_t elapsed_ms);

/** 查询显示模块是否可用。 */
bool DisplayController_IsEnabled(void);
