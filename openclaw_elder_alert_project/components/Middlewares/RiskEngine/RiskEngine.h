#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "SensorHub.h"

#define RISK_ENGINE_TEMP_HIGH_C             35.0f
#define RISK_ENGINE_HUMIDITY_HIGH_PERCENT   85.0f
#define RISK_ENGINE_MQ2_WARN_RAW           1500
#define RISK_ENGINE_LOW_LUX_THRESHOLD        30
#define RISK_ENGINE_NO_MOTION_REMIND_MS   30000U
#define RISK_ENGINE_MQ2_CONFIRM_MS         3000U
#define RISK_ENGINE_MQ2_FILTER_WINDOW         5U

#define RISK_ENGINE_RUN_MODE RISK_RUN_MODE_DEMO

typedef enum {
    RISK_LEVEL_NORMAL = 0,
    RISK_LEVEL_REMIND,
    RISK_LEVEL_WARNING,
    RISK_LEVEL_EMERGENCY,
} risk_level_t;

typedef enum {
    RISK_RUN_MODE_DEMO = 0,
    RISK_RUN_MODE_REAL,
} risk_run_mode_t;

typedef struct {
    uint32_t no_motion_remind_ms;
    uint32_t mq2_confirm_ms;
    uint8_t mq2_filter_window;
    int mq2_alarm_raw;
    float heat_temp_c;
    float heat_humidity_percent;
    uint16_t dark_lux_threshold;
} risk_config_t;

typedef struct {
    uint32_t now_ms;
    uint32_t inactive_ms;
    bool manual_sos_active;
    bool remind_timeout_active;
} risk_context_t;

typedef struct {
    risk_level_t level;
    uint8_t activity_score;
    uint8_t environment_score;
    uint8_t manual_score;
    uint8_t total_score;
    int mq2_filtered_raw;
    bool no_motion_timeout;
    bool low_light_no_motion;
    bool high_temperature;
    bool high_humidity;
    bool mq2_warning;
    bool manual_sos;
    bool remind_timeout;
} risk_result_t;

void RiskEngine_Evaluate(const sensor_hub_data_t *data,
                         const risk_context_t *context,
                         risk_result_t *result);
const char *RiskEngine_LevelToString(risk_level_t level);
void RiskEngine_BuildReasonString(const risk_result_t *result, char *buffer, size_t buffer_size);
void RiskEngine_LogResult(const risk_result_t *result);
