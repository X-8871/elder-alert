/**
 * @file AppController.c
 * @brief 应用层状态机实现。
 *
 * 状态流转规则（优先级从高到低）：
 *   1. 用户手动 SOS 键 → 立即进入 SOS（锁存，需用户确认解除）
 *   2. REMIND 状态超时未确认 → 升级为 ALARM
 *   3. RiskEngine 判定 WARNING / EMERGENCY → ALARM
 *   4. RiskEngine 判定 REMIND → REMIND
 *   5. 其余情况 → NORMAL
 */

#include "AppController.h"

#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "AlertController.h"
#include "InputController.h"

static const char *TAG = "AppController";

/* 这三个状态量构成了应用层状态机的核心上下文。 */
static bool s_initialized = false;
static TickType_t s_last_motion_tick = 0;
static TickType_t s_remind_start_tick = 0;
static app_state_t s_current_state = APP_STATE_NORMAL;
static bool s_sos_latched = false;
static bool s_remind_timeout_latched = false;
static uint32_t s_sos_trigger_count = 0;

static uint32_t get_remind_confirm_timeout_ms(void)
{
    if (APP_CONTROLLER_RUN_MODE == RISK_RUN_MODE_REAL) {
        return APP_CONTROLLER_REMIND_CONFIRM_TIMEOUT_MS_REAL;
    }

    return APP_CONTROLLER_REMIND_CONFIRM_TIMEOUT_MS_DEMO;
}

static esp_err_t apply_state(app_state_t next_state)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_current_state == next_state) {
        return ESP_OK;
    }

    ESP_LOGI(TAG,
             "state_change: %s -> %s",
             AppController_StateToString(s_current_state),
             AppController_StateToString(next_state));

    /* AppController 不直接操作 GPIO，而是把状态翻译给 AlertController。 */
    esp_err_t ret = ESP_OK;
    switch (next_state) {
    case APP_STATE_NORMAL:
        ret = AlertController_SetState(ALERT_STATE_NORMAL);
        break;
    case APP_STATE_REMIND:
        ret = AlertController_SetState(ALERT_STATE_REMIND);
        break;
    case APP_STATE_ALARM:
        ret = AlertController_SetState(ALERT_STATE_ALARM);
        break;
    case APP_STATE_SOS:
        ret = AlertController_SetState(ALERT_STATE_SOS);
        break;
    default:
        ret = ESP_ERR_INVALID_ARG;
        break;
    }

    if (ret == ESP_OK) {
        s_current_state = next_state;
        if (next_state == APP_STATE_REMIND) {
            s_remind_start_tick = xTaskGetTickCount();
            s_remind_timeout_latched = false;
        } else if (next_state == APP_STATE_NORMAL || next_state == APP_STATE_SOS) {
            s_remind_start_tick = 0;
            s_remind_timeout_latched = false;
        }
    }

    return ret;
}

const char *AppController_StateToString(app_state_t state)
{
    switch (state) {
    case APP_STATE_NORMAL:
        return "NORMAL";
    case APP_STATE_REMIND:
        return "REMIND";
    case APP_STATE_ALARM:
        return "ALARM";
    case APP_STATE_SOS:
        return "SOS";
    default:
        return "UNKNOWN";
    }
}

esp_err_t AppController_Init(void)
{
    /* 启动时认为“刚有过活动”，避免上电立刻被判定为长时间无人活动。 */
    s_last_motion_tick = xTaskGetTickCount();
    s_remind_start_tick = 0;
    s_current_state = APP_STATE_NORMAL;
    s_sos_latched = false;
    s_remind_timeout_latched = false;
    s_sos_trigger_count = 0;
    s_initialized = true;

    ESP_LOGI(TAG, "state_init: %s", AppController_StateToString(s_current_state));
    return AlertController_SetState(ALERT_STATE_NORMAL);
}

esp_err_t AppController_Process(const sensor_hub_data_t *sensor_data, const risk_result_t *risk_result)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (sensor_data == NULL || risk_result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!sensor_data->am312_ok || sensor_data->motion_detected) {
        /* AM312 不可用时不把未知活动状态误当作“长时间无活动”。 */
        s_last_motion_tick = xTaskGetTickCount();
        s_remind_timeout_latched = false;
    }

    if (s_sos_latched) {
        /* 手动 SOS 一旦锁存，优先级最高，直到用户确认清除。 */
        return apply_state(APP_STATE_SOS);
    }

    if (s_remind_timeout_latched) {
        return apply_state(APP_STATE_ALARM);
    }

    if (risk_result->level == RISK_LEVEL_EMERGENCY ||
        risk_result->level == RISK_LEVEL_WARNING) {
        /* WARNING/EMERGENCY 都统一映射成 ALARM。 */
        return apply_state(APP_STATE_ALARM);
    }

    if (risk_result->level == RISK_LEVEL_REMIND) {
        return apply_state(APP_STATE_REMIND);
    }

    return apply_state(APP_STATE_NORMAL);
}

esp_err_t AppController_Service(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    bool confirm_pressed = false;
    bool sos_pressed = false;
    esp_err_t ret = InputController_GetConfirmEvent(&confirm_pressed);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = InputController_GetSosEvent(&sos_pressed);
    if (ret != ESP_OK) {
        return ret;
    }

    if (sos_pressed) {
        /* SOS 键是锁存式事件，不会因为下一轮风险恢复正常就自动清掉。 */
        ++s_sos_trigger_count;
        s_sos_latched = true;
        ret = apply_state(APP_STATE_SOS);
        if (ret != ESP_OK) {
            return ret;
        }
        ESP_LOGW(TAG, "manual_sos_triggered");
    }

    if (confirm_pressed) {
        ESP_LOGI(TAG, "confirm key pressed");

        if (s_current_state == APP_STATE_REMIND) {
            /* REMIND 是“行为提醒”，用户确认后可以直接回 NORMAL。 */
            s_last_motion_tick = xTaskGetTickCount();
            ret = apply_state(APP_STATE_NORMAL);
            if (ret != ESP_OK) {
                return ret;
            }
            ESP_LOGI(TAG, "remind_cleared_by_user");
        } else if (s_current_state == APP_STATE_ALARM) {
            if (s_remind_timeout_latched) {
                /* 提醒超时升级的 ALARM，可由用户补确认后解除。 */
                s_remind_timeout_latched = false;
                s_last_motion_tick = xTaskGetTickCount();
                ret = apply_state(APP_STATE_NORMAL);
                if (ret != ESP_OK) {
                    return ret;
                }
                ESP_LOGI(TAG, "timeout_alarm_cleared_by_user");
                return AlertController_Update();
            }
            /* ALARM 下确认只静音本地提醒，不会篡改风险判断结果。 */
            ret = AlertController_Confirm();
            if (ret != ESP_OK) {
                return ret;
            }
        } else if (s_current_state == APP_STATE_SOS) {
            /* SOS 需要用户明确确认后才解除锁存。 */
            s_sos_latched = false;
            s_last_motion_tick = xTaskGetTickCount();
            ret = apply_state(APP_STATE_NORMAL);
            if (ret != ESP_OK) {
                return ret;
            }
            ESP_LOGI(TAG, "sos_cleared_by_user");
        }
    }

    if (s_current_state == APP_STATE_REMIND && s_remind_start_tick != 0) {
        TickType_t elapsed_ticks = xTaskGetTickCount() - s_remind_start_tick;
        uint32_t elapsed_ms = (uint32_t)(elapsed_ticks * portTICK_PERIOD_MS);
        uint32_t timeout_ms = get_remind_confirm_timeout_ms();
        if (elapsed_ms >= timeout_ms) {
            s_remind_timeout_latched = true;
            ret = apply_state(APP_STATE_ALARM);
            if (ret != ESP_OK) {
                return ret;
            }
            ESP_LOGW(TAG,
                     "remind_timeout_escalated elapsed_ms=%" PRIu32 " timeout_ms=%" PRIu32,
                     elapsed_ms,
                     timeout_ms);
        }
    }

    /* 每次 service 都刷新一次底层提示节拍，让闪灯/蜂鸣器按模式持续运行。 */
    return AlertController_Update();
}

app_state_t AppController_GetState(void)
{
    return s_current_state;
}

uint32_t AppController_GetInactiveTimeMs(void)
{
    if (!s_initialized) {
        return 0;
    }

    TickType_t inactive_ticks = xTaskGetTickCount() - s_last_motion_tick;
    return (uint32_t)(inactive_ticks * portTICK_PERIOD_MS);
}

bool AppController_IsSosLatched(void)
{
    return s_sos_latched;
}

bool AppController_IsRemindTimeoutLatched(void)
{
    return s_remind_timeout_latched;
}

uint32_t AppController_GetSosTriggerCount(void)
{
    return s_sos_trigger_count;
}
