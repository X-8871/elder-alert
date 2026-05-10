/**
 * @file RiskEngine.h
 * @brief 风险评估引擎，基于传感器数据和上下文计算综合风险等级。
 *
 * RiskEngine 采用多维度评分机制，从活动状态、环境指标、手动求助三个维度打分，
 * 最终汇总为 NORMAL / REMIND / WARNING / EMERGENCY 四级风险。
 * 支持 DEMO 和 REAL 两种运行模式，阈值可独立配置。
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "SensorHub.h"

/* 高温告警阈值 (°C) */
#define RISK_ENGINE_TEMP_HIGH_C             35.0f
#define RISK_ENGINE_HUMIDITY_HIGH_PERCENT   85.0f  /* 高湿度告警阈值 (%) */
#define RISK_ENGINE_MQ2_WARN_RAW           1500   /* MQ2 烟雾/可燃气体告警原始阈值 */
#define RISK_ENGINE_LOW_LUX_THRESHOLD        30    /* 低光照判定阈值 (lux) */
#define RISK_ENGINE_NO_MOTION_REMIND_MS   30000U   /* DEMO 模式无活动提醒时间 (ms) */
#define RISK_ENGINE_MQ2_CONFIRM_MS         3000U   /* MQ2 超阈值持续确认时间 (ms) */
#define RISK_ENGINE_MQ2_FILTER_WINDOW         5U   /* MQ2 滑动平均滤波窗口大小 */

#define RISK_ENGINE_RUN_MODE RISK_RUN_MODE_DEMO   /* 编译期选择运行模式 */

/** 风险等级，从低到高排列。 */
typedef enum {
    RISK_LEVEL_NORMAL = 0,  /* 无风险 */
    RISK_LEVEL_REMIND,      /* 轻度提醒（如长时间无活动） */
    RISK_LEVEL_WARNING,     /* 警告（如高温/高湿度/低光照无活动） */
    RISK_LEVEL_EMERGENCY,   /* 紧急（如烟雾报警或手动 SOS） */
} risk_level_t;

/** 运行模式：DEMO 用短阈值便于演示，REAL 用实际部署阈值。 */
typedef enum {
    RISK_RUN_MODE_DEMO = 0,
    RISK_RUN_MODE_REAL,
} risk_run_mode_t;

/** 运行时风险阈值配置，DEMO/REAL 模式各一份。 */
typedef struct {
    uint32_t no_motion_remind_ms;    /* 无活动提醒阈值 (ms) */
    uint32_t mq2_confirm_ms;         /* MQ2 超阈值持续确认时间 (ms) */
    uint8_t mq2_filter_window;       /* MQ2 滑动平均窗口 */
    int mq2_alarm_raw;               /* MQ2 告警原始值阈值 */
    float heat_temp_c;               /* 高温阈值 (°C) */
    float heat_humidity_percent;     /* 高湿度阈值 (%) */
    uint16_t dark_lux_threshold;     /* 低光照阈值 (lux) */
} risk_config_t;

/** 评估所需的外部上下文，由 AppController 每轮填入。 */
typedef struct {
    uint32_t now_ms;                 /* 当前系统运行时间 (ms) */
    uint32_t inactive_ms;            /* 自上次人体活动以来的时间 (ms) */
    bool manual_sos_active;          /* 用户是否已按下 SOS 键 */
    bool remind_timeout_active;      /* REMIND 状态是否已超时未确认 */
} risk_context_t;

/** 单次风险评估的完整输出。 */
typedef struct {
    risk_level_t level;              /* 最终风险等级 */
    uint8_t activity_score;          /* 活动维度得分 */
    uint8_t environment_score;       /* 环境维度得分 */
    uint8_t manual_score;            /* 手动求助维度得分 */
    uint8_t total_score;             /* 三维度总分 */
    int mq2_filtered_raw;            /* MQ2 滤波后的原始值 */
    bool no_motion_timeout;          /* 是否触发无活动超时 */
    bool low_light_no_motion;        /* 是否低光照且无活动 */
    bool high_temperature;           /* 是否高温 */
    bool high_humidity;              /* 是否高湿度 */
    bool mq2_warning;                /* MQ2 是否确认告警 */
    bool manual_sos;                 /* 是否手动 SOS */
    bool remind_timeout;             /* 提醒是否超时 */
} risk_result_t;

/** 综合传感器数据和上下文，计算各维度得分并确定风险等级。 */
void RiskEngine_Evaluate(const sensor_hub_data_t *data,
                         const risk_context_t *context,
                         risk_result_t *result);

/** 将风险等级枚举转为可读字符串。 */
const char *RiskEngine_LevelToString(risk_level_t level);

/** 根据评估结果拼接人类可读的风险原因描述（中文）。 */
void RiskEngine_BuildReasonString(const risk_result_t *result, char *buffer, size_t buffer_size);

/** 将评估结果以日志形式输出。 */
void RiskEngine_LogResult(const risk_result_t *result);
