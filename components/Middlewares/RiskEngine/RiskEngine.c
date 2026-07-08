/**
 * @file RiskEngine.c
 * @brief 风险评估引擎实现。
 *
 * 本模块只负责把传感器快照和 AppController 上下文转成风险结果。
 * 休息上下文、用户确认、REMIND/ALARM 的具体状态出口由 AppController 管理。
 */

#include "RiskEngine.h"

#include <string.h>

#include "esp_log.h"

static const char *TAG = "RiskEngine";

#define TEMP_HISTORY_SIZE 128U

typedef struct {
    uint32_t timestamp_ms;
    float temperature_c;
    bool valid;
} temp_sample_t;

static const risk_config_t s_demo_config = {
    .no_motion_remind_ms = RISK_ENGINE_NO_MOTION_REMIND_MS,
    .temp_confirm_ms = RISK_ENGINE_TEMP_CONFIRM_MS,
    .mq2_light_confirm_ms = RISK_ENGINE_MQ2_LIGHT_CONFIRM_MS,
    .mq2_alarm_confirm_ms = RISK_ENGINE_MQ2_ALARM_CONFIRM_MS,
    .mq2_clear_confirm_ms = RISK_ENGINE_MQ2_CLEAR_CONFIRM_MS,
    .mq2_filter_window = RISK_ENGINE_MQ2_FILTER_WINDOW,
    .mq2_light_raw = RISK_ENGINE_MQ2_LIGHT_RAW,
    .mq2_alarm_raw = RISK_ENGINE_MQ2_ALARM_RAW,
    .heat_temp_c = RISK_ENGINE_TEMP_HIGH_C,
    .dark_lux_threshold = 30U,
    .temp_rise_short_ms = RISK_ENGINE_TEMP_RISE_SHORT_MS,
    .temp_rise_short_c = RISK_ENGINE_TEMP_RISE_SHORT_C,
    .temp_rise_long_ms = RISK_ENGINE_TEMP_RISE_LONG_MS,
    .temp_rise_long_c = RISK_ENGINE_TEMP_RISE_LONG_C,
};

static const risk_config_t s_real_config = {
    .no_motion_remind_ms = 5U * 60U * 1000U,
    .temp_confirm_ms = 5U * 60U * 1000U,
    .mq2_light_confirm_ms = 10U * 1000U,
    .mq2_alarm_confirm_ms = 20U * 1000U,
    .mq2_clear_confirm_ms = 5U * 60U * 1000U,
    .mq2_filter_window = RISK_ENGINE_MQ2_FILTER_WINDOW,
    .mq2_light_raw = 1200,
    .mq2_alarm_raw = 1500,
    .heat_temp_c = 32.0f,
    .dark_lux_threshold = 20U,
    .temp_rise_short_ms = 60U * 1000U,
    .temp_rise_short_c = 4.0f,
    .temp_rise_long_ms = 3U * 60U * 1000U,
    .temp_rise_long_c = 5.0f,
};

static int s_mq2_samples[RISK_ENGINE_MQ2_FILTER_WINDOW] = {0};
static uint8_t s_mq2_sample_count = 0;
static uint8_t s_mq2_sample_index = 0;
static uint32_t s_mq2_light_since_ms = 0;
static uint32_t s_mq2_alarm_since_ms = 0;
static uint32_t s_mq2_clear_since_ms = 0;
static uint32_t s_high_temp_since_ms = 0;
static uint32_t s_temp_clear_since_ms = 0;
static temp_sample_t s_temp_history[TEMP_HISTORY_SIZE] = {0};
static uint8_t s_temp_history_index = 0;

/* 运行时模式（初始化为编译期默认值，可通过 RiskEngine_SetRunMode() 动态切换） */
static risk_run_mode_t s_run_mode = RISK_ENGINE_RUN_MODE;

static void reset_mq2_tracking(void);  /* 前向声明 */

static const risk_config_t *get_active_config(void)
{
    return s_run_mode == RISK_RUN_MODE_REAL ? &s_real_config : &s_demo_config;
}

void RiskEngine_SetRunMode(risk_run_mode_t mode)
{
    if (mode == s_run_mode) {
        return;
    }
    s_run_mode = mode;
    /* 切换模式后重置 MQ2 滤波跟踪，避免旧模式的累积数据影响新模式判断 */
    reset_mq2_tracking();
    ESP_LOGI(TAG, "run mode switched to %s", mode == RISK_RUN_MODE_REAL ? "REAL" : "DEMO");
}

risk_run_mode_t RiskEngine_GetRunMode(void)
{
    return s_run_mode;
}

static void reset_mq2_tracking(void)
{
    memset(s_mq2_samples, 0, sizeof(s_mq2_samples));
    s_mq2_sample_count = 0;
    s_mq2_sample_index = 0;
    s_mq2_light_since_ms = 0;
    s_mq2_alarm_since_ms = 0;
    s_mq2_clear_since_ms = 0;
}

static int update_mq2_filtered_raw(int raw, uint8_t filter_window)
{
    if (filter_window == 0 || filter_window > RISK_ENGINE_MQ2_FILTER_WINDOW) {
        filter_window = RISK_ENGINE_MQ2_FILTER_WINDOW;
    }

    s_mq2_samples[s_mq2_sample_index] = raw;
    s_mq2_sample_index = (uint8_t)((s_mq2_sample_index + 1U) % filter_window);
    if (s_mq2_sample_count < filter_window) {
        ++s_mq2_sample_count;
    }

    int32_t sum = 0;
    for (uint8_t i = 0; i < s_mq2_sample_count; ++i) {
        sum += s_mq2_samples[i];
    }

    return s_mq2_sample_count > 0 ? (int)(sum / s_mq2_sample_count) : raw;
}

static bool confirm_above_threshold(int value,
                                    int threshold,
                                    uint32_t confirm_ms,
                                    uint32_t now_ms,
                                    uint32_t *since_ms)
{
    if (since_ms == NULL) {
        return false;
    }
    if (value < threshold) {
        *since_ms = 0;
        return false;
    }
    if (*since_ms == 0) {
        *since_ms = now_ms;
        return confirm_ms == 0;
    }
    return (now_ms - *since_ms) >= confirm_ms;
}

static bool confirm_below_threshold(int value,
                                    int threshold,
                                    uint32_t confirm_ms,
                                    uint32_t now_ms,
                                    uint32_t *since_ms)
{
    if (since_ms == NULL) {
        return false;
    }
    if (value >= threshold) {
        *since_ms = 0;
        return false;
    }
    if (*since_ms == 0) {
        *since_ms = now_ms;
        return confirm_ms == 0;
    }
    return (now_ms - *since_ms) >= confirm_ms;
}

static bool evaluate_high_temperature(bool aht20_ok,
                                      float temperature_c,
                                      const risk_config_t *config,
                                      uint32_t now_ms)
{
    if (!aht20_ok || config == NULL || temperature_c < config->heat_temp_c) {
        s_high_temp_since_ms = 0;
        return false;
    }
    if (s_high_temp_since_ms == 0) {
        s_high_temp_since_ms = now_ms;
        return config->temp_confirm_ms == 0;
    }
    return (now_ms - s_high_temp_since_ms) >= config->temp_confirm_ms;
}

static bool evaluate_temperature_clear_stable(bool aht20_ok,
                                              float temperature_c,
                                              const risk_config_t *config,
                                              uint32_t now_ms)
{
    if (!aht20_ok || config == NULL) {
        s_temp_clear_since_ms = 0;
        return false;
    }
    if (temperature_c >= config->heat_temp_c) {
        s_temp_clear_since_ms = 0;
        return false;
    }
    if (s_temp_clear_since_ms == 0) {
        s_temp_clear_since_ms = now_ms;
        return config->temp_confirm_ms == 0;
    }
    return (now_ms - s_temp_clear_since_ms) >= config->temp_confirm_ms;
}

static void update_temperature_history(float temperature_c, uint32_t now_ms)
{
    s_temp_history[s_temp_history_index] = (temp_sample_t){
        .timestamp_ms = now_ms,
        .temperature_c = temperature_c,
        .valid = true,
    };
    s_temp_history_index = (uint8_t)((s_temp_history_index + 1U) % TEMP_HISTORY_SIZE);
}

static bool temperature_rise_reaches(float current_temp_c,
                                     uint32_t now_ms,
                                     uint32_t window_ms,
                                     float rise_threshold_c)
{
    const temp_sample_t *oldest = NULL;

    for (uint8_t i = 0; i < TEMP_HISTORY_SIZE; ++i) {
        const temp_sample_t *sample = &s_temp_history[i];
        if (!sample->valid || now_ms < sample->timestamp_ms) {
            continue;
        }
        if ((now_ms - sample->timestamp_ms) > window_ms) {
            continue;
        }
        if (oldest == NULL || sample->timestamp_ms < oldest->timestamp_ms) {
            oldest = sample;
        }
    }

    return oldest != NULL && (current_temp_c - oldest->temperature_c) >= rise_threshold_c;
}

static void append_reason(char *buffer, size_t buffer_size, const char *reason, bool *first)
{
    if (buffer == NULL || buffer_size == 0 || reason == NULL || first == NULL) {
        return;
    }

    size_t used = strlen(buffer);
    if (used >= buffer_size - 1U) {
        return;
    }

    if (!*first) {
        strncat(buffer, "；", buffer_size - used - 1U);
        used = strlen(buffer);
        if (used >= buffer_size - 1U) {
            return;
        }
    }
    strncat(buffer, reason, buffer_size - used - 1U);
    *first = false;
}

void RiskEngine_Evaluate(const sensor_hub_data_t *data,
                         const risk_context_t *context,
                         risk_result_t *result)
{
    if (result == NULL) {
        return;
    }

    *result = (risk_result_t){
        .level = RISK_LEVEL_NORMAL,
    };

    if (data == NULL || context == NULL) {
        result->level = RISK_LEVEL_EMERGENCY;
        result->environment_score = 3;
        result->total_score = 3;
        return;
    }

    const risk_config_t *config = get_active_config();
    result->rest_context_active = context->rest_context_active;

    if (!context->rest_context_active && data->ld2410b_ok) {
        result->static_presence_no_motion = data->ld2410b_presence &&
                                            !data->ld2410b_moving_target &&
                                            context->inactive_ms >= config->no_motion_remind_ms;
        result->no_motion_timeout = result->static_presence_no_motion;
    }

    if (data->aht20_ok) {
        update_temperature_history(data->aht_temperature, context->now_ms);
    }
    result->high_temperature = evaluate_high_temperature(data->aht20_ok,
                                                         data->aht_temperature,
                                                         config,
                                                         context->now_ms);
    result->temperature_clear_stable = evaluate_temperature_clear_stable(data->aht20_ok,
                                                                         data->aht_temperature,
                                                                         config,
                                                                         context->now_ms);

    if (data->mq2_ok) {
        result->mq2_filtered_raw = update_mq2_filtered_raw(data->mq2_raw, config->mq2_filter_window);
        result->mq2_light_warning = confirm_above_threshold(result->mq2_filtered_raw,
                                                            config->mq2_light_raw,
                                                            config->mq2_light_confirm_ms,
                                                            context->now_ms,
                                                            &s_mq2_light_since_ms);
        result->mq2_high_alarm = confirm_above_threshold(result->mq2_filtered_raw,
                                                         config->mq2_alarm_raw,
                                                         config->mq2_alarm_confirm_ms,
                                                         context->now_ms,
                                                         &s_mq2_alarm_since_ms);
        result->mq2_clear_stable = confirm_below_threshold(result->mq2_filtered_raw,
                                                           config->mq2_light_raw,
                                                           config->mq2_clear_confirm_ms,
                                                           context->now_ms,
                                                           &s_mq2_clear_since_ms);
        result->mq2_temp_rise_alarm =
            result->mq2_light_warning &&
            data->aht20_ok &&
            (temperature_rise_reaches(data->aht_temperature,
                                      context->now_ms,
                                      config->temp_rise_short_ms,
                                      config->temp_rise_short_c) ||
             temperature_rise_reaches(data->aht_temperature,
                                      context->now_ms,
                                      config->temp_rise_long_ms,
                                      config->temp_rise_long_c));
    } else {
        reset_mq2_tracking();
    }

    result->manual_sos = context->manual_sos_active;
    result->remind_timeout = context->remind_timeout_active;
    result->fall_detected = context->fall_detected;

    if (result->no_motion_timeout) {
        result->activity_score += 1;
    }
    if (result->high_temperature || result->mq2_light_warning) {
        result->environment_score += 1;
    }
    if (result->mq2_high_alarm || result->mq2_temp_rise_alarm) {
        result->environment_score += 3;
    }
    if (result->manual_sos) {
        result->manual_score = 3;
    }
    if (result->remind_timeout) {
        result->activity_score += 2;
    }

    result->total_score = result->activity_score +
                          result->environment_score +
                          result->manual_score;

    if (result->manual_sos || result->mq2_high_alarm || result->mq2_temp_rise_alarm || result->fall_detected) {
        result->level = RISK_LEVEL_EMERGENCY;
    } else if (result->remind_timeout) {
        result->level = RISK_LEVEL_WARNING;
    } else if (result->no_motion_timeout ||
               result->high_temperature ||
               result->mq2_light_warning) {
        result->level = RISK_LEVEL_REMIND;
    }
}

const char *RiskEngine_LevelToString(risk_level_t level)
{
    switch (level) {
    case RISK_LEVEL_NORMAL:
        return "NORMAL";
    case RISK_LEVEL_REMIND:
        return "REMIND";
    case RISK_LEVEL_WARNING:
        return "WARNING";
    case RISK_LEVEL_EMERGENCY:
        return "EMERGENCY";
    default:
        return "UNKNOWN";
    }
}

void RiskEngine_BuildReasonString(const risk_result_t *result, char *buffer, size_t buffer_size)
{
    if (buffer == NULL || buffer_size == 0) {
        return;
    }

    buffer[0] = '\0';
    if (result == NULL) {
        return;
    }

    bool first = true;
    if (result->manual_sos) {
        append_reason(buffer, buffer_size, "老人按下SOS求助键，已启动本地报警并上报云端", &first);
    }
    if (result->remind_timeout) {
        append_reason(buffer, buffer_size, "本地提醒后未收到确认，已升级为报警并上报云端", &first);
    }
    if (result->static_presence_no_motion) {
        append_reason(buffer, buffer_size, "检测到有人存在但长时间没有明显活动，已提醒老人确认是否安全", &first);
    }
    if (result->high_temperature) {
        append_reason(buffer, buffer_size, "室内温度较高，已提醒通风降温和补水", &first);
    }
    if (result->mq2_light_warning) {
        append_reason(buffer, buffer_size, "检测到烟雾或可燃气体轻度持续异常，已提醒现场确认", &first);
    }
    if (result->mq2_high_alarm) {
        append_reason(buffer, buffer_size, "检测到烟雾或可燃气体异常，已启动本地报警并上报云端", &first);
    }
    if (result->mq2_temp_rise_alarm) {
        append_reason(buffer, buffer_size, "烟雾或可燃气体异常并伴随温度明显上升，已启动本地报警并上报云端", &first);
    }
    if (result->fall_detected) {
        append_reason(buffer, buffer_size, "检测到人体距离骤降且持续静止在地面高度，疑似跌倒，已启动报警并上报云端", &first);
    }
}

void RiskEngine_LogResult(const risk_result_t *result)
{
    if (result == NULL) {
        ESP_LOGE(TAG, "risk result is NULL");
        return;
    }

    char reason_buf[192] = {0};
    RiskEngine_BuildReasonString(result, reason_buf, sizeof(reason_buf));

    ESP_LOGI(TAG,
             "risk_level=%s total=%u activity=%u environment=%u manual=%u mq2_filtered=%d reason=%s",
             RiskEngine_LevelToString(result->level),
             result->total_score,
             result->activity_score,
             result->environment_score,
             result->manual_score,
             result->mq2_filtered_raw,
             reason_buf[0] != '\0' ? reason_buf : "none");
}
