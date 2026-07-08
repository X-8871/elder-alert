/**
 * @file BSP_Alert.h
 * @brief LED + 蜂鸣器声光提示驱动——支持多种闪烁/鸣叫模式。
 *
 * 声光模式与系统状态对应：
 *   OFF     → 全关
 *   NORMAL  → LED 按 1000ms 节拍慢闪
 *   REMIND  → LED + 蜂鸣器按 500ms 节拍同步翻转
 *   ALARM   → LED + 蜂鸣器按 250ms 节拍
 *   SOS     → LED + 蜂鸣器按 120ms 节拍（最高频）
 *
 * 需要在主循环中持续调用 BSP_Alert_Update() 以推进闪烁节拍。
 */

#pragma once

#include <stdbool.h>

#include "driver/gpio.h"
#include "esp_err.h"

typedef enum {
    BSP_ALERT_MODE_OFF = 0,
    BSP_ALERT_MODE_NORMAL,
    BSP_ALERT_MODE_REMIND,
    BSP_ALERT_MODE_ALARM,
    BSP_ALERT_MODE_SOS,
} bsp_alert_mode_t;

/**
 * @brief 初始化 LED 和蜂鸣器 GPIO 引脚。
 * @param led_gpio    LED 控制引脚（本项目 GPIO10）
 * @param buzzer_gpio 蜂鸣器控制引脚（本项目 GPIO9）
 * @return ESP_ERR_INVALID_ARG 如果两个 GPIO 相同或参数无效
 */
esp_err_t BSP_Alert_Init(gpio_num_t led_gpio, gpio_num_t buzzer_gpio);

/** 切换声光输出模式——立即生效，并重置闪烁节拍起点 */
esp_err_t BSP_Alert_SetMode(bsp_alert_mode_t mode);

/** 推进闪烁/鸣叫节拍——在主循环中周期调用（建议每 100ms 一次） */
esp_err_t BSP_Alert_Update(void);

/** 直接控制 LED 和蜂鸣器（会覆盖当前模式为 OFF） */
esp_err_t BSP_Alert_SetOutputs(bool led_on, bool buzzer_on);

bsp_alert_mode_t BSP_Alert_GetMode(void);
bool BSP_Alert_IsInitialized(void);
