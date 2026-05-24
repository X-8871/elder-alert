/**
 * @file AppController.c
 * @brief 应用层状态机实现。
 *
 * 当前实现对应根目录《风险判断与状态显示规则表 v1》：
 * - 休息场景只作为内部上下文，不单独显示或上报。
 * - 不同 REMIND 原因有不同出口，高温不会自动升级 ALARM。
 * - MQ2 高危险/温升联动报警不能仅靠 OK 判正常。
 */

#include "AppController.h"

#include <inttypes.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "AlertController.h"
#include "InputController.h"

static const char *TAG = "AppController";

#define DISTANCE_EVENT_MAX 128U

typedef enum {
    REMIND_CAUSE_NONE = 0,
    REMIND_CAUSE_NO_MOTION,
    REMIND_CAUSE_HIGH_TEMP,
    REMIND_CAUSE_MQ2_LIGHT,
} remind_cause_t;

typedef enum {
    ALARM_CAUSE_NONE = 0,
    ALARM_CAUSE_NO_MOTION_TIMEOUT,
    ALARM_CAUSE_MQ2_LIGHT_TIMEOUT,
    ALARM_CAUSE_MQ2_HARD,
    ALARM_CAUSE_MQ2_TEMP_RISE,
} alarm_cause_t;

typedef struct {
    uint32_t timestamp_ms;
    uint16_t delta_cm;
    bool valid;
} distance_event_t;

static bool s_initialized = false;
static TickType_t s_last_motion_tick = 0;
static TickType_t s_remind_start_tick = 0;
static app_state_t s_current_state = APP_STATE_NORMAL;
static bool s_sos_latched = false;
static bool s_remind_timeout_latched = false;
static bool s_rest_context_active = false;
static uint32_t s_sos_trigger_count = 0;
static uint32_t s_low_light_since_ms = 0;
static uint32_t s_high_temp_cooldown_until_ms = 0;
static uint32_t s_mq2_alarm_cooldown_until_ms = 0;
static remind_cause_t s_remind_cause = REMIND_CAUSE_NONE;
static alarm_cause_t s_alarm_cause = ALARM_CAUSE_NONE;
static bool s_last_distance_valid = false;
static uint16_t s_last_distance_cm = 0;
static distance_event_t s_distance_events[DISTANCE_EVENT_MAX] = {0};
static uint8_t s_distance_event_index = 0;
static const char *s_pending_voice_prompt_key = NULL;

static uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static uint32_t get_remind_confirm_timeout_ms(void)
{
    return APP_CONTROLLER_RUN_MODE == RISK_RUN_MODE_REAL
               ? APP_CONTROLLER_REMIND_CONFIRM_TIMEOUT_MS_REAL
               : APP_CONTROLLER_REMIND_CONFIRM_TIMEOUT_MS_DEMO;
}

static uint32_t get_high_temp_cooldown_ms(void)
{
    return APP_CONTROLLER_RUN_MODE == RISK_RUN_MODE_REAL
               ? APP_CONTROLLER_HIGH_TEMP_COOLDOWN_MS_REAL
               : APP_CONTROLLER_HIGH_TEMP_COOLDOWN_MS_DEMO;
}

static uint32_t get_mq2_alarm_cooldown_ms(void)
{
    return APP_CONTROLLER_RUN_MODE == RISK_RUN_MODE_REAL
               ? APP_CONTROLLER_MQ2_ALARM_COOLDOWN_MS_REAL
               : APP_CONTROLLER_MQ2_ALARM_COOLDOWN_MS_DEMO;
}

static uint16_t get_rest_enter_lux(void)
{
    return APP_CONTROLLER_RUN_MODE == RISK_RUN_MODE_REAL
               ? APP_CONTROLLER_REST_ENTER_LUX_REAL
               : APP_CONTROLLER_REST_ENTER_LUX_DEMO;
}

static uint16_t get_rest_exit_lux(void)
{
    return APP_CONTROLLER_RUN_MODE == RISK_RUN_MODE_REAL
               ? APP_CONTROLLER_REST_EXIT_LUX_REAL
               : APP_CONTROLLER_REST_EXIT_LUX_DEMO;
}

static uint32_t get_rest_enter_ms(void)
{
    return APP_CONTROLLER_RUN_MODE == RISK_RUN_MODE_REAL
               ? APP_CONTROLLER_REST_ENTER_MS_REAL
               : APP_CONTROLLER_REST_ENTER_MS_DEMO;
}

static uint32_t get_distance_change_window_ms(void)
{
    return APP_CONTROLLER_RUN_MODE == RISK_RUN_MODE_REAL
               ? APP_CONTROLLER_DISTANCE_CHANGE_WINDOW_MS_REAL
               : APP_CONTROLLER_DISTANCE_CHANGE_WINDOW_MS_DEMO;
}

static uint8_t get_distance_change_count_threshold(void)
{
    return APP_CONTROLLER_RUN_MODE == RISK_RUN_MODE_REAL
               ? APP_CONTROLLER_DISTANCE_CHANGE_COUNT_REAL
               : APP_CONTROLLER_DISTANCE_CHANGE_COUNT_DEMO;
}

static uint32_t get_low_light_activity_window_ms(void)
{
    return APP_CONTROLLER_RUN_MODE == RISK_RUN_MODE_REAL
               ? APP_CONTROLLER_LOW_LIGHT_ACTIVITY_WINDOW_MS_REAL
               : APP_CONTROLLER_LOW_LIGHT_ACTIVITY_WINDOW_MS_DEMO;
}

static uint16_t get_low_light_activity_sum_cm(void)
{
    return APP_CONTROLLER_RUN_MODE == RISK_RUN_MODE_REAL
               ? APP_CONTROLLER_LOW_LIGHT_ACTIVITY_SUM_CM_REAL
               : APP_CONTROLLER_LOW_LIGHT_ACTIVITY_SUM_CM_DEMO;
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
            s_remind_cause = REMIND_CAUSE_NONE;
            if (next_state == APP_STATE_NORMAL) {
                s_alarm_cause = ALARM_CAUSE_NONE;
            }
        }
    }

    return ret;
}

static esp_err_t apply_state_with_voice_prompt(app_state_t next_state, const char *voice_prompt_key)
{
    app_state_t previous_state = s_current_state;
    esp_err_t ret = apply_state(next_state);
    if (ret == ESP_OK && previous_state != next_state && voice_prompt_key != NULL) {
        s_pending_voice_prompt_key = voice_prompt_key;
    }
    return ret;
}

static void set_remind_cause(remind_cause_t cause)
{
    if (s_remind_cause == cause) {
        return;
    }

    s_remind_cause = cause;
    if (s_current_state == APP_STATE_REMIND) {
        s_remind_start_tick = xTaskGetTickCount();
        s_remind_timeout_latched = false;
    }
}

static void add_distance_event(uint32_t timestamp_ms, uint16_t delta_cm)
{
    s_distance_events[s_distance_event_index] = (distance_event_t){
        .timestamp_ms = timestamp_ms,
        .delta_cm = delta_cm,
        .valid = true,
    };
    s_distance_event_index = (uint8_t)((s_distance_event_index + 1U) % DISTANCE_EVENT_MAX);
}

static bool get_person_distance_cm(const sensor_hub_data_t *sensor_data, uint16_t *distance_cm)
{
    if (sensor_data == NULL || distance_cm == NULL ||
        !sensor_data->ld2410b_ok || !sensor_data->ld2410b_presence) {
        return false;
    }

    if (sensor_data->ld2410b_detection_distance_cm > 0) {
        *distance_cm = sensor_data->ld2410b_detection_distance_cm;
        return true;
    }
    if (sensor_data->ld2410b_moving_distance_cm > 0) {
        *distance_cm = sensor_data->ld2410b_moving_distance_cm;
        return true;
    }
    if (sensor_data->ld2410b_stationary_distance_cm > 0) {
        *distance_cm = sensor_data->ld2410b_stationary_distance_cm;
        return true;
    }
    return false;
}

static uint8_t count_distance_events(uint32_t current_ms, uint32_t window_ms)
{
    uint8_t count = 0;
    for (uint8_t i = 0; i < DISTANCE_EVENT_MAX; ++i) {
        const distance_event_t *event = &s_distance_events[i];
        if (event->valid && current_ms >= event->timestamp_ms &&
            (current_ms - event->timestamp_ms) <= window_ms) {
            ++count;
        }
    }
    return count;
}

static uint32_t sum_distance_events(uint32_t current_ms, uint32_t window_ms)
{
    uint32_t sum_cm = 0;
    for (uint8_t i = 0; i < DISTANCE_EVENT_MAX; ++i) {
        const distance_event_t *event = &s_distance_events[i];
        if (event->valid && current_ms >= event->timestamp_ms &&
            (current_ms - event->timestamp_ms) <= window_ms) {
            sum_cm += event->delta_cm;
        }
    }
    return sum_cm;
}

static bool update_distance_tracking(const sensor_hub_data_t *sensor_data, uint32_t current_ms)
{
    uint16_t distance_cm = 0;
    if (!get_person_distance_cm(sensor_data, &distance_cm)) {
        s_last_distance_valid = false;
        return false;
    }

    if (!s_last_distance_valid) {
        s_last_distance_valid = true;
        s_last_distance_cm = distance_cm;
        return false;
    }

    uint16_t delta_cm = (uint16_t)abs((int)distance_cm - (int)s_last_distance_cm);
    s_last_distance_cm = distance_cm;
    if (delta_cm < APP_CONTROLLER_DISTANCE_CHANGE_MIN_CM) {
        return false;
    }

    add_distance_event(current_ms, delta_cm);
    return true;
}

static bool distance_changed_often(uint32_t current_ms)
{
    return count_distance_events(current_ms, get_distance_change_window_ms()) >=
           get_distance_change_count_threshold();
}

static bool low_light_obvious_activity(uint32_t current_ms, const sensor_hub_data_t *sensor_data)
{
    return sensor_data != NULL &&
           sensor_data->ld2410b_ok &&
           sensor_data->ld2410b_presence &&
           distance_changed_often(current_ms) &&
           sum_distance_events(current_ms, get_low_light_activity_window_ms()) >=
               get_low_light_activity_sum_cm();
}

static bool no_motion_recovery_ready(uint32_t current_ms, const sensor_hub_data_t *sensor_data)
{
    if (sensor_data == NULL) {
        return false;
    }

    bool activity_detected = sensor_data->ld2410b_ok && sensor_data->ld2410b_moving_target;
    return activity_detected && distance_changed_often(current_ms);
}

static void update_rest_context(const sensor_hub_data_t *sensor_data,
                                uint32_t current_ms,
                                bool obvious_activity)
{
    if (sensor_data == NULL || !sensor_data->bh1750_ok) {
        s_low_light_since_ms = 0;
        if (s_rest_context_active) {
            s_rest_context_active = false;
            s_last_motion_tick = xTaskGetTickCount();
            ESP_LOGW(TAG, "rest_context_exit_by_bh1750_fault");
        }
        return;
    }

    bool low_light = sensor_data->lux <= get_rest_enter_lux();
    if (low_light) {
        if (s_low_light_since_ms == 0) {
            s_low_light_since_ms = current_ms;
        } else if (!s_rest_context_active &&
                   (current_ms - s_low_light_since_ms) >= get_rest_enter_ms()) {
            s_rest_context_active = true;
            s_last_motion_tick = xTaskGetTickCount();
            ESP_LOGI(TAG, "rest_context_enter lux=%u", (unsigned)sensor_data->lux);
        }
    } else {
        s_low_light_since_ms = 0;
    }

    if (!s_rest_context_active) {
        return;
    }

    if (low_light_obvious_activity(current_ms, sensor_data)) {
        s_rest_context_active = false;
        s_last_motion_tick = xTaskGetTickCount();
        ESP_LOGI(TAG, "rest_context_exit_by_low_light_activity");
        return;
    }

    if (sensor_data->lux >= get_rest_exit_lux() &&
        obvious_activity &&
        distance_changed_often(current_ms)) {
        s_rest_context_active = false;
        s_last_motion_tick = xTaskGetTickCount();
        ESP_LOGI(TAG, "rest_context_exit_by_bright_activity lux=%u", (unsigned)sensor_data->lux);
    }
}

static bool high_temp_cooldown_active(uint32_t current_ms)
{
    return s_high_temp_cooldown_until_ms != 0 &&
           current_ms < s_high_temp_cooldown_until_ms;
}

static bool mq2_alarm_cooldown_active(uint32_t current_ms)
{
    return s_mq2_alarm_cooldown_until_ms != 0 &&
           current_ms < s_mq2_alarm_cooldown_until_ms;
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
    s_last_motion_tick = xTaskGetTickCount();
    s_remind_start_tick = 0;
    s_current_state = APP_STATE_NORMAL;
    s_sos_latched = false;
    s_remind_timeout_latched = false;
    s_rest_context_active = false;
    s_sos_trigger_count = 0;
    s_low_light_since_ms = 0;
    s_high_temp_cooldown_until_ms = 0;
    s_mq2_alarm_cooldown_until_ms = 0;
    s_remind_cause = REMIND_CAUSE_NONE;
    s_alarm_cause = ALARM_CAUSE_NONE;
    s_last_distance_valid = false;
    s_last_distance_cm = 0;
    for (uint8_t i = 0; i < DISTANCE_EVENT_MAX; ++i) {
        s_distance_events[i] = (distance_event_t){0};
    }
    s_distance_event_index = 0;
    s_pending_voice_prompt_key = NULL;
    s_initialized = true;

    ESP_LOGI(TAG, "state_init: %s", AppController_StateToString(s_current_state));
    return AlertController_SetState(ALERT_STATE_NORMAL);
}

esp_err_t AppController_UpdateContext(const sensor_hub_data_t *sensor_data)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (sensor_data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t current_ms = now_ms();
    bool distance_event = update_distance_tracking(sensor_data, current_ms);
    bool obvious_activity = (sensor_data->ld2410b_ok && sensor_data->ld2410b_moving_target) ||
                            distance_event;
    bool no_person_in_mmwave_area = sensor_data->ld2410b_ok && !sensor_data->ld2410b_presence;
    bool activity_unknown = !sensor_data->ld2410b_ok;

    update_rest_context(sensor_data, current_ms, obvious_activity);

    if (s_rest_context_active ||
        obvious_activity ||
        no_person_in_mmwave_area ||
        activity_unknown) {
        s_last_motion_tick = xTaskGetTickCount();
        if (obvious_activity || no_person_in_mmwave_area || activity_unknown) {
            s_remind_timeout_latched = false;
        }
    }

    return ESP_OK;
}

esp_err_t AppController_Process(const sensor_hub_data_t *sensor_data, const risk_result_t *risk_result)
{
    (void)sensor_data;
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (risk_result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t current_ms = now_ms();

    if (s_sos_latched) {
        return apply_state(APP_STATE_SOS);
    }

    if (s_current_state == APP_STATE_ALARM) {
        if (risk_result->mq2_temp_rise_alarm) {
            s_alarm_cause = ALARM_CAUSE_MQ2_TEMP_RISE;
        } else if (risk_result->mq2_high_alarm) {
            s_alarm_cause = ALARM_CAUSE_MQ2_HARD;
        }
        if ((s_alarm_cause == ALARM_CAUSE_MQ2_HARD &&
             risk_result->mq2_clear_stable &&
             !risk_result->mq2_high_alarm &&
             !risk_result->mq2_temp_rise_alarm) ||
            (s_alarm_cause == ALARM_CAUSE_MQ2_TEMP_RISE &&
             risk_result->mq2_clear_stable &&
             risk_result->temperature_clear_stable &&
             !risk_result->mq2_high_alarm &&
             !risk_result->mq2_temp_rise_alarm &&
             !risk_result->high_temperature) ||
            (s_alarm_cause == ALARM_CAUSE_NO_MOTION_TIMEOUT &&
             !risk_result->no_motion_timeout) ||
            (s_alarm_cause == ALARM_CAUSE_MQ2_LIGHT_TIMEOUT &&
             risk_result->mq2_clear_stable)) {
            return apply_state(APP_STATE_NORMAL);
        }
        return apply_state(APP_STATE_ALARM);
    }

    if (!mq2_alarm_cooldown_active(current_ms) &&
        s_mq2_alarm_cooldown_until_ms != 0 &&
        risk_result->mq2_light_warning &&
        !risk_result->mq2_clear_stable) {
        s_alarm_cause = ALARM_CAUSE_MQ2_LIGHT_TIMEOUT;
        s_remind_timeout_latched = true;
        return apply_state_with_voice_prompt(APP_STATE_ALARM, "mq2_mild_timeout_alarm");
    }
    if (risk_result->mq2_clear_stable) {
        s_mq2_alarm_cooldown_until_ms = 0;
    }

    if (risk_result->mq2_temp_rise_alarm) {
        s_alarm_cause = ALARM_CAUSE_MQ2_TEMP_RISE;
        return apply_state_with_voice_prompt(APP_STATE_ALARM, "mq2_temp_alarm");
    }

    if (risk_result->mq2_high_alarm) {
        s_alarm_cause = ALARM_CAUSE_MQ2_HARD;
        return apply_state_with_voice_prompt(APP_STATE_ALARM, "mq2_danger_alarm");
    }

    if (s_current_state == APP_STATE_REMIND) {
        if (s_remind_cause == REMIND_CAUSE_NO_MOTION) {
            if (no_motion_recovery_ready(current_ms, sensor_data)) {
                s_last_motion_tick = xTaskGetTickCount();
                return apply_state_with_voice_prompt(APP_STATE_NORMAL, "risk_cleared_normal");
            }
            return apply_state(APP_STATE_REMIND);
        }
        if (s_remind_cause == REMIND_CAUSE_HIGH_TEMP && !risk_result->high_temperature) {
            return apply_state_with_voice_prompt(APP_STATE_NORMAL, "risk_cleared_normal");
        }
        if (s_remind_cause == REMIND_CAUSE_MQ2_LIGHT) {
            if (risk_result->mq2_clear_stable) {
                return apply_state_with_voice_prompt(APP_STATE_NORMAL, "risk_cleared_normal");
            }
            return apply_state(APP_STATE_REMIND);
        }
    }

    if (risk_result->mq2_light_warning) {
        set_remind_cause(REMIND_CAUSE_MQ2_LIGHT);
        return apply_state_with_voice_prompt(APP_STATE_REMIND, "mq2_mild_remind");
    }

    if (risk_result->no_motion_timeout) {
        set_remind_cause(REMIND_CAUSE_NO_MOTION);
        return apply_state_with_voice_prompt(APP_STATE_REMIND, "no_motion_remind");
    }

    if (risk_result->high_temperature && !high_temp_cooldown_active(current_ms)) {
        set_remind_cause(REMIND_CAUSE_HIGH_TEMP);
        return apply_state_with_voice_prompt(APP_STATE_REMIND, "high_temp_remind");
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
        ++s_sos_trigger_count;
        s_sos_latched = true;
        s_alarm_cause = ALARM_CAUSE_NONE;
        ret = apply_state(APP_STATE_SOS);
        if (ret != ESP_OK) {
            return ret;
        }
        ESP_LOGW(TAG, "manual_sos_triggered");
    }

    if (confirm_pressed) {
        ESP_LOGI(TAG, "confirm key pressed");

        if (s_current_state == APP_STATE_REMIND) {
            if (s_remind_cause == REMIND_CAUSE_HIGH_TEMP) {
                s_high_temp_cooldown_until_ms = now_ms() + get_high_temp_cooldown_ms();
                s_last_motion_tick = xTaskGetTickCount();
                ret = apply_state_with_voice_prompt(APP_STATE_NORMAL, "user_confirm_normal");
                ESP_LOGI(TAG, "high_temp_remind_cleared_by_user cooldown_ms=%" PRIu32,
                         get_high_temp_cooldown_ms());
            } else if (s_remind_cause == REMIND_CAUSE_NO_MOTION) {
                s_last_motion_tick = xTaskGetTickCount();
                ret = apply_state_with_voice_prompt(APP_STATE_NORMAL, "user_confirm_normal");
                ESP_LOGI(TAG, "no_motion_remind_cleared_by_user");
            } else if (s_remind_cause == REMIND_CAUSE_MQ2_LIGHT) {
                ret = AlertController_Confirm();
                ESP_LOGI(TAG, "mq2_light_remind_acknowledged");
            }
            if (ret != ESP_OK) {
                return ret;
            }
        } else if (s_current_state == APP_STATE_ALARM) {
            if (s_alarm_cause == ALARM_CAUSE_NO_MOTION_TIMEOUT) {
                s_remind_timeout_latched = false;
                s_last_motion_tick = xTaskGetTickCount();
                ret = apply_state_with_voice_prompt(APP_STATE_NORMAL, "user_confirm_normal");
                ESP_LOGI(TAG, "no_motion_alarm_cleared_by_user");
            } else if (s_alarm_cause == ALARM_CAUSE_MQ2_LIGHT_TIMEOUT) {
                s_remind_timeout_latched = false;
                s_mq2_alarm_cooldown_until_ms = now_ms() + get_mq2_alarm_cooldown_ms();
                ret = apply_state_with_voice_prompt(APP_STATE_NORMAL, "user_confirm_normal");
                ESP_LOGI(TAG, "mq2_light_alarm_cleared_by_user cooldown_ms=%" PRIu32,
                         get_mq2_alarm_cooldown_ms());
            } else {
                ret = AlertController_Confirm();
                ESP_LOGI(TAG, "hard_alarm_acknowledged_without_state_clear");
            }
            if (ret != ESP_OK) {
                return ret;
            }
        } else if (s_current_state == APP_STATE_SOS) {
            s_sos_latched = false;
            s_last_motion_tick = xTaskGetTickCount();
            ret = apply_state_with_voice_prompt(APP_STATE_NORMAL, "user_confirm_normal");
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
        bool can_escalate = s_remind_cause == REMIND_CAUSE_NO_MOTION ||
                            s_remind_cause == REMIND_CAUSE_MQ2_LIGHT;
        if (can_escalate && elapsed_ms >= timeout_ms) {
            s_remind_timeout_latched = true;
            s_alarm_cause = s_remind_cause == REMIND_CAUSE_MQ2_LIGHT
                                ? ALARM_CAUSE_MQ2_LIGHT_TIMEOUT
                                : ALARM_CAUSE_NO_MOTION_TIMEOUT;
            ret = apply_state_with_voice_prompt(
                APP_STATE_ALARM,
                s_remind_cause == REMIND_CAUSE_MQ2_LIGHT
                    ? "mq2_mild_timeout_alarm"
                    : "no_motion_timeout_alarm");
            if (ret != ESP_OK) {
                return ret;
            }
            ESP_LOGW(TAG,
                     "remind_timeout_escalated cause=%d elapsed_ms=%" PRIu32 " timeout_ms=%" PRIu32,
                     (int)s_remind_cause,
                     elapsed_ms,
                     timeout_ms);
        }
    }

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

bool AppController_IsRestContextActive(void)
{
    return s_rest_context_active;
}

uint32_t AppController_GetSosTriggerCount(void)
{
    return s_sos_trigger_count;
}

const char *AppController_TakePendingVoicePromptKey(void)
{
    const char *key = s_pending_voice_prompt_key;
    s_pending_voice_prompt_key = NULL;
    return key;
}
