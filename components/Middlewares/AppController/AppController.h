/**
 * @file AppController.h
 * @brief 应用层状态机——管理 NORMAL / REMIND / ALARM / SOS 四种业务状态。
 *
 * 【学弟必读：状态机是什么？】
 * 想象一个交通信号灯：它只能在"红→绿→黄→红"这几个状态之间切换，
 * 不会突然出现"蓝色"。AppController 做的事情类似：
 * - 系统在 NORMAL 状态下运行
 * - 传感器发现异常 → 状态改为 REMIND（提醒老人确认一下）
 * - 老人很久没按确认键 → 状态升级为 ALARM（告警，通知远程家属）
 * - 老人按下 SOS 键 → 不管当前什么状态，直接进入 SOS（最高优先级）
 *
 * 【四个状态的含义】
 * - NORMAL：一切正常，绿色
 * - REMIND：有异常需要老人确认，橙色（温和提醒）
 * - ALARM：较严重或超时未确认，红色（需要外部关注）
 * - SOS：老人主动求助，最紧急，深红
 *
 * 【休息上下文 (REST)】
 * 当环境光照很低 + 长时间没有人活动时，系统认为老人在睡觉。
 * "REST" 不是第五个状态，而是内部的"标记"——
 * 在休息期间，不会因为长时间无活动而进入 REMIND（避免半夜吵醒老人）。
 * TFT 屏幕上也不会显示 REST，只显示当前的 NORMAL/REMIND/ALARM/SOS。
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
uint32_t AppController_GetSosTriggerCount(void);   /* SOS 累计触发次数 */
const char *AppController_StateToString(app_state_t state);

/**
 * @brief 取出待播放的状态播报事件 key。
 *        返回 NULL 表示没有新的待播报事件。
 *        调用即消费——同一事件不会返回两次。
 */
const char *AppController_TakePendingVoicePromptKey(void);
