/**
 * @file BSP_Alert.h
 * @brief LED + 蜂鸣器声光提示驱动，支持多种闪烁/鸣叫模式。
 *
 * 模式说明：
 *   OFF    - 全关
 *   NORMAL - LED 常亮，蜂鸣器关
 *   REMIND - LED 慢闪（500ms），同步蜂鸣器
 *   ALARM  - LED 快闪（250ms），蜂鸣器同步鸣叫
 *   SOS    - LED 最快闪（120ms），蜂鸣器同步鸣叫
 *
 * 需在主循环中持续调用 BSP_Alert_Update() 以推进闪烁节拍。
 */

#pragma once

#include <stdbool.h>

#include "driver/gpio.h"
#include "esp_err.h"

/** 声光输出模式枚举。 */
typedef enum {
    BSP_ALERT_MODE_OFF = 0,   /* 全关 */
    BSP_ALERT_MODE_NORMAL,    /* LED 常亮 */
    BSP_ALERT_MODE_REMIND,    /* LED 慢闪 + 蜂鸣器同步 */
    BSP_ALERT_MODE_ALARM,     /* LED 快闪 + 蜂鸣器鸣叫 */
    BSP_ALERT_MODE_SOS,       /* LED 最快闪 + 蜂鸣器持续鸣叫 */
} bsp_alert_mode_t;

esp_err_t BSP_Alert_Init(gpio_num_t led_gpio, gpio_num_t buzzer_gpio);
esp_err_t BSP_Alert_SetMode(bsp_alert_mode_t mode);

/** 推进闪烁/鸣叫节拍，主循环中周期调用。 */
esp_err_t BSP_Alert_Update(void);

/** 直接控制 LED 和蜂鸣器输出（会切换为 OFF 模式）。 */
esp_err_t BSP_Alert_SetOutputs(bool led_on, bool buzzer_on);
bsp_alert_mode_t BSP_Alert_GetMode(void);
bool BSP_Alert_IsInitialized(void);
