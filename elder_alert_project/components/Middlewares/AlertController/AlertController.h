/**
 * @file AlertController.h
 * @brief 声光提示控制器，根据应用状态驱动 LED 闪烁和蜂鸣器鸣叫。
 *
 * AlertController 是 AppController 与底层 BSP_Alert 之间的桥梁：
 *   - 接收 AppController 设置的提醒状态（NORMAL/REMIND/ALARM/SOS）
 *   - 翻译为 BSP_Alert 的输出模式（不同频率的闪烁和鸣叫）
 *   - 支持用户确认后静音本地声光（不改变业务状态）
 */

#pragma once

#include "driver/gpio.h"
#include "esp_err.h"
#include "RiskEngine.h"

#define ALERT_CONTROLLER_LED_GPIO    GPIO_NUM_10
#define ALERT_CONTROLLER_BUZZER_GPIO GPIO_NUM_9

/** 提醒状态枚举，与 AppController 的应用状态一一对应。 */
typedef enum {
    ALERT_STATE_NORMAL = 0, /* 无提示 */
    ALERT_STATE_REMIND,     /* 慢闪提醒 */
    ALERT_STATE_ALARM,      /* 快闪 + 蜂鸣告警 */
    ALERT_STATE_SOS,        /* 最快闪 + 持续蜂鸣 */
} alert_state_t;

esp_err_t AlertController_Init(void);

/** 由上层直接设置提醒状态，切换底层声光输出模式。 */
esp_err_t AlertController_SetState(alert_state_t state);

/** 兼容接口：根据 RiskEngine 结果自动映射为 NORMAL / REMIND / ALARM。 */
esp_err_t AlertController_ApplyRisk(const risk_result_t *risk_result);

/** 用户确认告警后静音本地声光，不改变业务上的风险状态。 */
esp_err_t AlertController_Confirm(void);

/** 周期刷新 LED / 蜂鸣器输出节拍，需在主循环中持续调用。 */
esp_err_t AlertController_Update(void);
