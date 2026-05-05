#include "AlertController.h"

#include "esp_log.h"

#include "BSP_Alert.h"

static const char *TAG = "AlertController";
static bool s_initialized = false;
/* 用户在 ALARM 状态按确认键后，只静音本地声光，不改变系统风险状态。 */
static bool s_alarm_muted = false;

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

esp_err_t AlertController_Update(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
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
