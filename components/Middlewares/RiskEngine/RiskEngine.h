/**
 * @file RiskEngine.h
 * @brief 风险评估引擎——把传感器数据 + 上下文信息转成风险结论。
 *
 * 【学弟必读：RiskEngine 做什么？】
 * RiskEngine 就像一个"评分裁判"，它不控制任何硬件，只做计算。
 *
 * 输入：
 * - sensor_hub_data_t（传感器快照）
 * - risk_context_t（上下文：手动SOS？休息中？多久没活动了？）
 *
 * 输出：
 * - risk_result_t（风险等级 + 各维度得分 + 具体原因标志位）
 *
 * 【风险等级四级制】
 * NORMAL    = 0  正常，没事
 * REMIND    = 1  轻度提醒（高温/轻度烟雾/长时间无活动）
 * WARNING   = 2  警告级别（提醒超时未确认）
 * EMERGENCY = 3  紧急（手动SOS / MQ2高危险 / MQ2+温升联动）
 *
 * 【DEMO 和 REAL 双参数表】
 * DEMO：比赛演示用，阈值短（秒级），方便评委快速看到功能
 * REAL：真实居家部署用，阈值长（分钟级），避免误报骚扰老人
 *
 * 【MQ2 为什么需要"持续确认"？】
 * 燃气灶点火瞬间可能冒一点烟，如果一超阈值就报警就是误报。
 * 持续确认 = 连续 N 秒都超阈值才报警，过滤掉瞬间波动。
 * 配合滑动平均滤波（5帧窗口），进一步平滑数据。
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "SensorHub.h"

/* ========== DEMO 参数（实验版/演示版）========== */
#define RISK_ENGINE_TEMP_HIGH_C             30.0f  /* 高温阈值 30°C */
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
} risk_result_t;

void RiskEngine_Evaluate(const sensor_hub_data_t *data,
                         const risk_context_t *context,
                         risk_result_t *result);

const char *RiskEngine_LevelToString(risk_level_t level);
void RiskEngine_BuildReasonString(const risk_result_t *result, char *buffer, size_t buffer_size);
void RiskEngine_LogResult(const risk_result_t *result);
