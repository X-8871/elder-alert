/**
 * @file BSP_Alert.h
 * @brief LED + 蜂鸣器声光提示驱动——支持多种闪烁/鸣叫模式。
 *
 * 【学弟必读：声光提示的四种模式】
 * 本模块负责把系统状态"翻译"成老人能感知到的声光信号：
 *
 *   OFF     → 全关（没事的时候不打扰老人）
 *   NORMAL  → LED 常亮绿灯（表示系统在正常工作）
 *   REMIND  → LED 慢闪 + 蜂鸣器间歇（提醒老人"该确认一下了"，节奏温和）
 *   ALARM   → LED 快闪 + 蜂鸣器较急促（告警级别，比提醒更紧迫）
 *   SOS     → LED 最快闪 + 蜂鸣器最急促（120ms 节拍，紧急求助）
 *
 * 【为什么要用 FreeRTOS tick 来控制闪烁？】
 * 如果直接用 vTaskDelay() 延时来控制闪烁节奏，整个系统都会被阻塞住。
 * 用 tick（系统时钟节拍）记录"上次翻转的时刻"，
 * 然后在 Update() 中判断是否到了该翻转的时间，这样不阻塞其他任务。
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
