/**
 * @file RiskEngine.h
 * @brief 风险评估引擎——把传感器数据 + 上下文信息转成风险结论。
 *
 * 输入：sensor_hub_data_t + risk_context_t
 * 输出：risk_result_t（风险等级 + 各维度得分 + 原因标志位）
 *
 * 风险等级：NORMAL(0) / REMIND(1) / WARNING(2) / EMERGENCY(3)
 * 支持 DEMO（秒级阈值）和 REAL（分钟级阈值）双参数表。
 * 编译期通过 RISK_ENGINE_RUN_MODE 选择默认模式，运行时可通过 RiskEngine_SetRunMode() 动态切换。
 * MQ2 采用持续确认 + 滑动平均滤波以过滤瞬间波动。
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "SensorHub.h"

/* ========== DEMO 参数（实验版/演示版）========== */
#define RISK_ENGINE_TEMP_HIGH_C             31.0f  /* 高温阈值 31°C */
#define RISK_ENGINE_MQ2_LIGHT_RAW          1000   /* MQ2 轻度异常阈值 */
#define RISK_ENGINE_MQ2_ALARM_RAW          1300   /* MQ2 高危险阈值 */
#define RISK_ENGINE_NO_MOTION_REMIND_MS   30000U  /* 无活动提醒：30秒 */
#define RISK_ENGINE_TEMP_CONFIRM_MS       30000U  /* 高温持续确认：30秒 */
#define RISK_ENGINE_MQ2_LIGHT_CONFIRM_MS   3000U  /* MQ2 轻度持续确认：3秒 */
#define RISK_ENGINE_MQ2_ALARM_CONFIRM_MS   5000U  /* MQ2 高危险持续确认：5秒 */
#define RISK_ENGINE_MQ2_CLEAR_CONFIRM_MS  10000U  /* MQ2 回落稳定确认：10秒 */
#define RISK_ENGINE_MQ2_FILTER_WINDOW         5U  /* 滑动平均窗口大小 */
#define RISK_ENGINE_TEMP_RISE_SHORT_MS     30000U  /* 短窗口温升：30秒 */
#define RISK_ENGINE_TEMP_RISE_SHORT_C        2.0f  /* 短窗口升温阈值：+2°C */
#define RISK_ENGINE_TEMP_RISE_LONG_MS      60000U  /* 长窗口温升：60秒 */
#define RISK_ENGINE_TEMP_RISE_LONG_C         3.0f  /* 长窗口升温阈值：+3°C */

#define RISK_ENGINE_RUN_MODE RISK_RUN_MODE_DEMO   /* 编译期选择模式 */

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

/** 风险阈值配置——DEMO 和 REAL 各存一份 */
typedef struct {
    uint32_t no_motion_remind_ms;
    uint32_t temp_confirm_ms;
    uint32_t mq2_light_confirm_ms;
    uint32_t mq2_alarm_confirm_ms;
    uint32_t mq2_clear_confirm_ms;
    uint8_t mq2_filter_window;
    int mq2_light_raw;
    int mq2_alarm_raw;
    float heat_temp_c;
    uint16_t dark_lux_threshold;
    uint32_t temp_rise_short_ms;
    float temp_rise_short_c;
    uint32_t temp_rise_long_ms;
    float temp_rise_long_c;
} risk_config_t;

/** 评估所需的上下文——由 AppController 每轮填入 */
typedef struct {
    uint32_t now_ms;                 /* 当前系统运行时间 (ms) */
    uint32_t inactive_ms;            /* 多久没检测到活动了 (ms) */
    bool manual_sos_active;          /* 用户是否已按下 SOS */
    bool remind_timeout_active;      /* REMIND 是否已超时 */
    bool rest_context_active;        /* 是否在休息上下文中 */
    bool fall_detected;              /* AppController 跌倒检测：距离骤降+持续静止 */
} risk_context_t;

/** 单次评估的完整输出 */
typedef struct {
    risk_level_t level;
    uint8_t activity_score;          /* 活动维度得分 */
    uint8_t environment_score;       /* 环境维度得分 */
    uint8_t manual_score;            /* 手动求助维度得分 */
    uint8_t total_score;             /* 总分 */
    int mq2_filtered_raw;            /* MQ2 滑动平均后的值 */

    /* 各风险原因标志位——AppController 根据这些决定状态如何转移 */
    bool no_motion_timeout;
    bool rest_context_active;
    bool high_temperature;
    bool mq2_light_warning;
    bool mq2_high_alarm;
    bool mq2_temp_rise_alarm;
    bool mq2_clear_stable;
    bool temperature_clear_stable;
    bool manual_sos;
    bool remind_timeout;
    bool static_presence_no_motion;  /* 毫米波：有人存在但长时间无明显活动 */
    bool fall_detected;              /* 毫米波：距离骤降+持续静止，疑似跌倒 */
} risk_result_t;

void RiskEngine_Evaluate(const sensor_hub_data_t *data,
                         const risk_context_t *context,
                         risk_result_t *result);

const char *RiskEngine_LevelToString(risk_level_t level);
void RiskEngine_BuildReasonString(const risk_result_t *result, char *buffer, size_t buffer_size);
void RiskEngine_LogResult(const risk_result_t *result);

/** 运行时切换 DEMO/REAL 模式（由 Agent 命令触发） */
void RiskEngine_SetRunMode(risk_run_mode_t mode);

/** 获取当前运行时模式 */
risk_run_mode_t RiskEngine_GetRunMode(void);
