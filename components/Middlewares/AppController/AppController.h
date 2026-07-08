/**
 * @file AppController.h
 * @brief 应用层状态机——管理 NORMAL / REMIND / ALARM / SOS 四种业务状态。
 *
 * 状态优先级：SOS > ALARM > REMIND > NORMAL。
 * 休息上下文(REST)：低光 + 长时间无活动时认为老人在睡觉，休息期间不因无活动触发 REMIND。
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "RiskEngine.h"
#include "SensorHub.h"

/* ---- DEMO/REAL 双参数体系 ---- */
/* DEMO = 比赛演示用短阈值（秒级），REAL = 真实部署用长阈值（分钟级） */
#define APP_CONTROLLER_REMIND_CONFIRM_TIMEOUT_MS_DEMO 15000U             /* 15秒 */
#define APP_CONTROLLER_REMIND_CONFIRM_TIMEOUT_MS_REAL (30U * 60U * 1000U) /* 30分钟 */
#define APP_CONTROLLER_HIGH_TEMP_COOLDOWN_MS_DEMO (60U * 1000U)           /* 1分钟冷却 */
#define APP_CONTROLLER_HIGH_TEMP_COOLDOWN_MS_REAL (10U * 60U * 1000U)     /* 10分钟冷却 */
#define APP_CONTROLLER_MQ2_ALARM_COOLDOWN_MS_DEMO 30000U                  /* 30秒冷却 */
#define APP_CONTROLLER_MQ2_ALARM_COOLDOWN_MS_REAL (5U * 60U * 1000U)      /* 5分钟冷却 */

/* 休息上下文阈值 */
#define APP_CONTROLLER_REST_ENTER_LUX_DEMO 30U                             /* <=30 lux 算低光 */
#define APP_CONTROLLER_REST_ENTER_LUX_REAL 20U
#define APP_CONTROLLER_REST_ENTER_MS_DEMO (60U * 1000U)                   /* 低光持续60秒进入休息 */
#define APP_CONTROLLER_REST_ENTER_MS_REAL (10U * 60U * 1000U)
#define APP_CONTROLLER_REST_EXIT_LUX_DEMO 60U                              /* >=60 lux 退出休息 */
#define APP_CONTROLLER_REST_EXIT_LUX_REAL 50U

/* 距离变化追踪参数 */
#define APP_CONTROLLER_DISTANCE_CHANGE_MIN_CM 20U                          /* 最小距离变化 20cm 才记录 */
#define APP_CONTROLLER_DISTANCE_CHANGE_COUNT_DEMO 2U                       /* DEMO: 窗口内2次变化=明显活动 */
#define APP_CONTROLLER_DISTANCE_CHANGE_COUNT_REAL 3U
#define APP_CONTROLLER_DISTANCE_CHANGE_WINDOW_MS_DEMO 30000U               /* 统计窗口30秒 */
#define APP_CONTROLLER_DISTANCE_CHANGE_WINDOW_MS_REAL (3U * 60U * 1000U)
#define APP_CONTROLLER_LOW_LIGHT_ACTIVITY_SUM_CM_DEMO 50U                  /* 低光下累计变化>=50cm */
#define APP_CONTROLLER_LOW_LIGHT_ACTIVITY_SUM_CM_REAL 100U
#define APP_CONTROLLER_LOW_LIGHT_ACTIVITY_WINDOW_MS_DEMO 30000U
#define APP_CONTROLLER_LOW_LIGHT_ACTIVITY_WINDOW_MS_REAL (60U * 1000U)

/* 跌倒检测参数（基于 LD2410B 距离骤降 + 持续静止） */
#define APP_CONTROLLER_FALL_DROP_THRESHOLD_CM 80U    /* 距离骤降>=80cm 判为可能跌倒 */
#define APP_CONTROLLER_FALL_CONFIRM_MS_DEMO 10000U   /* DEMO: 静止10秒确认跌倒 */
#define APP_CONTROLLER_FALL_CONFIRM_MS_REAL 30000U   /* REAL: 静止30秒确认跌倒 */
#define APP_CONTROLLER_FALL_CLEAR_ACTIVITY_MS 5000U  /* 持续活动5秒清除跌倒标记 */

#define APP_CONTROLLER_RUN_MODE RISK_ENGINE_RUN_MODE  /* 与 RiskEngine 共用运行模式 */

/** 应用状态枚举——按风险从低到高排列 */
typedef enum {
    APP_STATE_NORMAL = 0,  /* 一切正常 */
    APP_STATE_REMIND,      /* 轻度提醒，等待用户确认 */
    APP_STATE_ALARM,       /* 告警，声光提示并上报云端 */
    APP_STATE_SOS,         /* 紧急求助（用户手动触发） */
} app_state_t;

/** 初始化状态机——系统从 NORMAL 起步 */
esp_err_t AppController_Init(void);

/**
 * @brief 在每次 RiskEngine 评估之前调用，更新上下文信息。
 *        包括：休息上下文判断、距离变化追踪、最近活动时间。
 */
esp_err_t AppController_UpdateContext(const sensor_hub_data_t *sensor_data);

/** 根据风险结果推进状态机——这是主循环中"决策"的核心入口 */
esp_err_t AppController_Process(const sensor_hub_data_t *sensor_data, const risk_result_t *risk_result);

/**
 * @brief 高频服务函数（须在 100ms 内层循环中调用）。
 *        处理：确认键/SOS键事件、REMIND超时升级、冷却计时、声光刷新。
 */
esp_err_t AppController_Service(void);

/* ---- 状态查询接口（供其他中间件模块调用）---- */

app_state_t AppController_GetState(void);
uint32_t AppController_GetInactiveTimeMs(void);   /* 距上次检测到活动过了多少 ms */
bool AppController_IsSosLatched(void);             /* SOS 是否处于锁存状态 */
bool AppController_IsRemindTimeoutLatched(void);   /* REMIND 是否已超时升级 */
bool AppController_IsRestContextActive(void);      /* 是否处于休息上下文 */
bool AppController_IsFallDetected(void);           /* 跌倒检测是否确认 */
uint32_t AppController_GetSosTriggerCount(void);   /* SOS 累计触发次数 */
const char *AppController_StateToString(app_state_t state);

/**
 * @brief 取出待播放的状态播报事件 key。
 *        返回 NULL 表示没有新的待播报事件。
 *        调用即消费——同一事件不会返回两次。
 */
const char *AppController_TakePendingVoicePromptKey(void);

/**
 * @brief 远程确认（Agent 命令调用）。
 *        设置一个待确认标志，下次 AppController_Service() 时消费，
 *        效果等同按下确认键。
 */
void AppController_RemoteConfirm(void);

/**
 * @brief 运行时切换 DEMO/REAL 模式（由 Agent 命令触发）。
 *        内部会同步调用 RiskEngine_SetRunMode()。
 */
void AppController_SetRunMode(risk_run_mode_t mode);
