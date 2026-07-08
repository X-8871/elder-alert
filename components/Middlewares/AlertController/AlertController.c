/**
 * @file AlertController.c
 * @brief 声光提示控制器实现，将应用状态翻译为 BSP_Alert 的输出模式。
 *
 * 支持用户确认后静音：静音期间保持业务上的 ALARM 状态，但本地声光切回 NORMAL。
 * 状态变更时自动清除静音标记（SOS/REMIND/NORMAL 切换时重置）。
 */

#include "AlertController.h"

#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "BSP_Alert.h"

static const char *TAG = "AlertController";
static bool s_initialized = false;
/* 用户在 ALARM 状态按确认键后，只静音本地声光，不改变系统风险状态。 */
static bool s_alarm_muted = false;

/* Agent beep_once 支持：非阻塞短蜂鸣 */
static bool s_beep_pending = false;
static TickType_t s_beep_until_tick = 0;
#define ALERT_CONTROLLER_BEEP_MS 200U

esp_err_t AlertController_Init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    /* AlertController 的唯一硬件依赖就是 BSP_Alert。 */
    esp_err_t ret = BSP_Alert_Init(ALERT_CONTROLLER_LED_GPIO, ALERT_CONTROLLER_BUZZER_GPIO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "alert BSP init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_initialized = true;
    return BSP_Alert_SetMode(BSP_ALERT_MODE_NORMAL);
}

esp_err_t AlertController_ApplyRisk(const risk_result_t *risk_result)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (risk_result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (risk_result->level == RISK_LEVEL_WARNING ||
        risk_result->level == RISK_LEVEL_EMERGENCY) {
        return AlertController_SetState(ALERT_STATE_ALARM);
    }

    if (risk_result->level == RISK_LEVEL_REMIND) {
        return AlertController_SetState(ALERT_STATE_REMIND);
    }

    return AlertController_SetState(ALERT_STATE_NORMAL);
}

esp_err_t AlertController_Confirm(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    s_alarm_muted = true;
    ESP_LOGI(TAG, "user_confirmed: local alarm muted");
    /* 静音的实现方式是把底层模式临时切回 NORMAL。 */
    return BSP_Alert_SetMode(BSP_ALERT_MODE_NORMAL);
}

esp_err_t AlertController_BeepOnce(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    s_beep_pending = true;
    s_beep_until_tick = xTaskGetTickCount() + pdMS_TO_TICKS(ALERT_CONTROLLER_BEEP_MS);
    ESP_LOGI(TAG, "beep_once requested");
    return ESP_OK;
}

esp_err_t AlertController_Update(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Agent beep_once 优先：蜂鸣器强制开启，跳过正常闪烁逻辑 */
    if (s_beep_pending) {
        if (xTaskGetTickCount() < s_beep_until_tick) {
            /* 直接拉低蜂鸣器引脚（低电平有效），保持 LED 当前状态 */
            gpio_set_level(ALERT_CONTROLLER_BUZZER_GPIO, 0);
            return ESP_OK;
        }
        s_beep_pending = false;
        /* 蜂鸣时间结束，落入正常 Update 恢复正确输出 */
    }

    return BSP_Alert_Update();
}

esp_err_t AlertController_SetState(alert_state_t state)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    switch (state) {
    case ALERT_STATE_NORMAL:
        s_alarm_muted = false;
        ESP_LOGI(TAG, "alert_mode=NORMAL");
        return BSP_Alert_SetMode(BSP_ALERT_MODE_NORMAL);

    case ALERT_STATE_REMIND:
        s_alarm_muted = false;
        ESP_LOGI(TAG, "alert_mode=REMIND");
        return BSP_Alert_SetMode(BSP_ALERT_MODE_REMIND);

    case ALERT_STATE_ALARM:
        if (s_alarm_muted) {
            /* 如果用户已经确认过，则仍保留业务上的 ALARM，但本地提示保持静音。 */
            ESP_LOGW(TAG, "alert_mode=MUTED_ALARM");
            return BSP_Alert_SetMode(BSP_ALERT_MODE_NORMAL);
        }

        ESP_LOGW(TAG, "alert_mode=ALARM");
        return BSP_Alert_SetMode(BSP_ALERT_MODE_ALARM);

    case ALERT_STATE_SOS:
        s_alarm_muted = false;
        ESP_LOGW(TAG, "alert_mode=SOS");
        return BSP_Alert_SetMode(BSP_ALERT_MODE_SOS);

    default:
        return ESP_ERR_INVALID_ARG;
    }
}
