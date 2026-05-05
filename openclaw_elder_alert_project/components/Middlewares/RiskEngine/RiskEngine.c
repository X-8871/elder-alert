#include "RiskEngine.h"

#include <string.h>

#include "esp_log.h"

static const char *TAG = "RiskEngine";

static const risk_config_t s_demo_config = {
    .no_motion_remind_ms = RISK_ENGINE_NO_MOTION_REMIND_MS,
    .mq2_confirm_ms = RISK_ENGINE_MQ2_CONFIRM_MS,
    .mq2_filter_window = RISK_ENGINE_MQ2_FILTER_WINDOW,
    .mq2_alarm_raw = RISK_ENGINE_MQ2_WARN_RAW,
    .heat_temp_c = RISK_ENGINE_TEMP_HIGH_C,
    .heat_humidity_percent = RISK_ENGINE_HUMIDITY_HIGH_PERCENT,
    .dark_lux_threshold = RISK_ENGINE_LOW_LUX_THRESHOLD,
};

static const risk_config_t s_real_config = {
    .no_motion_remind_ms = 30U * 60U * 1000U,
    .mq2_confirm_ms = 10000U,
    .mq2_filter_window = RISK_ENGINE_MQ2_FILTER_WINDOW,
    .mq2_alarm_raw = RISK_ENGINE_MQ2_WARN_RAW,
    .heat_temp_c = 32.0f,
    .heat_humidity_percent = 75.0f,
    .dark_lux_threshold = 20U,
};

static int s_mq2_samples[RISK_ENGINE_MQ2_FILTER_WINDOW] = {0};
static uint8_t s_mq2_sample_count = 0;
static uint8_t s_mq2_sample_index = 0;
static uint32_t s_mq2_over_threshold_since_ms = 0;

static const risk_config_t *get_active_config(void)
{
    if (RISK_ENGINE_RUN_MODE == RISK_RUN_MODE_REAL) {
        return &s_real_config;
    }
    return &s_demo_config;
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

static bool evaluate_mq2_confirmed(int filtered_raw,
                                   const risk_config_t *config,
                                   uint32_t now_ms)
{
    if (config == NULL) {
        return false;
    }

    if (filtered_raw < config->mq2_alarm_raw) {
        s_mq2_over_threshold_since_ms = 0;
        return false;
    }

    if (s_mq2_over_threshold_since_ms == 0) {
        s_mq2_over_threshold_since_ms = now_ms;
        return config->mq2_confirm_ms == 0;
    }

    return (now_ms - s_mq2_over_threshold_since_ms) >= config->mq2_confirm_ms;
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
    result->no_motion_timeout = context->inactive_ms >= config->no_motion_remind_ms;
    result->low_light_no_motion = result->no_motion_timeout &&
                                  data->lux <= config->dark_lux_threshold;
    result->high_temperature = data->aht_temperature >= config->heat_temp_c;
    result->high_humidity = data->humidity >= config->heat_humidity_percent;
    result->mq2_filtered_raw = update_mq2_filtered_raw(data->mq2_raw, config->mq2_filter_window);
    result->mq2_warning = evaluate_mq2_confirmed(result->mq2_filtered_raw, config, context->now_ms);
    result->manual_sos = context->manual_sos_active;
    result->remind_timeout = context->remind_timeout_active;

    if (result->no_motion_timeout) {
        result->activity_score += 1;
    }
    if (result->low_light_no_motion) {
        result->activity_score += 1;
    }
    if (result->high_temperature) {
        result->environment_score += 1;
    }
    if (result->high_humidity) {
        result->environment_score += 1;
    }
    if (result->mq2_warning) {
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

    if (result->manual_sos || result->mq2_warning) {
        result->level = RISK_LEVEL_EMERGENCY;
    } else if (result->remind_timeout) {
        result->level = RISK_LEVEL_WARNING;
    } else if (result->low_light_no_motion ||
               result->environment_score > 0 ||
               (result->activity_score > 0 && result->environment_score > 0)) {
        result->level = RISK_LEVEL_WARNING;
    } else if (result->no_motion_timeout) {
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
    if (result->no_motion_timeout) {
        append_reason(buffer, buffer_size, "长时间未检测到明显活动，已提醒老人确认是否安全", &first);
    }
    if (result->low_light_no_motion) {
        append_reason(buffer, buffer_size, "长时间无活动且光线较暗，已提醒老人确认安全", &first);
    }
    if (result->high_temperature) {
        append_reason(buffer, buffer_size, "室内温度较高，已提醒通风降温和补水", &first);
    }
    if (result->high_humidity) {
        append_reason(buffer, buffer_size, "室内湿度较高，已提醒通风并关注舒适度", &first);
    }
    if (result->mq2_warning) {
        append_reason(buffer, buffer_size, "检测到烟雾或可燃气体异常，已启动本地报警并上报云端", &first);
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
