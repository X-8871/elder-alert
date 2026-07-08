/**
 * @file ui.h
 * @brief LVGL 界面逻辑接口（纯 UI，不涉及底层驱动）
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "DisplayController.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化所有 LVGL 控件，构建控件树
 */
esp_err_t ui_init(void);

/**
 * @brief 更新 UI 界面文本与颜色状态
 * @param app_state 当前的系统状态（正常/警报等）
 * @param online    当前是否连接 Wi-Fi
 * @param time_text 格式化好的时间字符串（如 "14:30"）
 * @param sensor_text 格式化好的传感器信息字符串
 */
esp_err_t ui_update(app_state_t app_state, bool online, const char *time_text, const char *sensor_text);

/**
 * @brief 周期性调用，用于处理 UI 层的自定义动画（如呼吸灯效果）
 * @param app_state 当前状态，用于判断是否需要开启动画
 * @param elapsed_ms 距上次调用的时间
 */
void ui_service_animations(app_state_t app_state, uint32_t elapsed_ms);

/**
 * @brief 在屏幕中央显示一条临时消息（Agent命令使用）
 * @param message 要显示的消息文本
 * @param duration_seconds 显示持续时间（秒）
 */
void ui_show_temporary_message(const char *message, int duration_seconds);

/** 切换语音交互或确认反馈动画。 */
void ui_set_activity(display_activity_t activity);

#ifdef __cplusplus
}
#endif
