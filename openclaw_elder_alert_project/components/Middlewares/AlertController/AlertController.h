#pragma once

#include "driver/gpio.h"
#include "esp_err.h"
#include "RiskEngine.h"

#define ALERT_CONTROLLER_LED_GPIO    GPIO_NUM_10
#define ALERT_CONTROLLER_BUZZER_GPIO GPIO_NUM_9

/* AlertController 只管理提醒状态，不关心具体传感器细节。 */
typedef enum {
    ALERT_STATE_NORMAL = 0,
    ALERT_STATE_REMIND,
    ALERT_STATE_ALARM,
    ALERT_STATE_SOS,
} alert_state_t;

esp_err_t AlertController_Init(void);

/* 由上层直接设置提醒状态。 */
esp_err_t AlertController_SetState(alert_state_t state);

/* 兼容接口：根据 RiskEngine 的结果切换 NORMAL / ALARM。 */
esp_err_t AlertController_ApplyRisk(const risk_result_t *risk_result);

/* 用户确认告警后，静音本地提醒。 */
esp_err_t AlertController_Confirm(void);

/* 周期刷新 LED / 蜂鸣器输出。 */
esp_err_t AlertController_Update(void);
