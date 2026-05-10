/**
 * @file AppController.h
 * @brief 应用层状态机，统一管理 REMIND / ALARM / SOS 等业务状态的切换与用户交互响应。
 *
 * AppController 是 Middlewares 层的核心调度器：
 *   - 接收 SensorHub 采集的传感器数据和 RiskEngine 的风险评估结果
 *   - 驱动状态在 NORMAL → REMIND → ALARM → SOS 之间流转
 *   - 处理用户确认键和 SOS 键的输入事件
 *   - 将最终状态翻译给 AlertController 执行本地声光提示
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "RiskEngine.h"
#include "SensorHub.h"

/* REMIND 状态下等待用户确认的超时时间，超时后升级为 ALARM。 */
#define APP_CONTROLLER_NO_MOTION_TIMEOUT_MS 30000
#define APP_CONTROLLER_REMIND_CONFIRM_TIMEOUT_MS_DEMO 15000U   /* 演示模式：15 秒 */
#define APP_CONTROLLER_REMIND_CONFIRM_TIMEOUT_MS_REAL 300000U /* 实际部署：5 分钟 */
#define APP_CONTROLLER_RUN_MODE RISK_ENGINE_RUN_MODE          /* 与 RiskEngine 共用运行模式宏 */

/** 应用层状态枚举，由低到高表示风险等级递增。 */
typedef enum {
    APP_STATE_NORMAL = 0, /* 一切正常 */
    APP_STATE_REMIND,     /* 轻度提醒，等待用户确认 */
    APP_STATE_ALARM,      /* 告警，声光提示并上报云端 */
    APP_STATE_SOS,        /* 紧急求助，用户手动触发或提醒超时升级 */
} app_state_t;

esp_err_t AppController_Init(void);

/** 根据传感器数据和风险结果更新应用状态。每轮主循环调用一次。 */
esp_err_t AppController_Process(const sensor_hub_data_t *sensor_data, const risk_result_t *risk_result);

/** 处理用户输入事件（确认键/SOS 键）和提醒超时升级逻辑，并刷新底层提示输出。 */
esp_err_t AppController_Service(void);

app_state_t AppController_GetState(void);

/** 返回自上次检测到人体活动以来的毫秒数。 */
uint32_t AppController_GetInactiveTimeMs(void);

bool AppController_IsSosLatched(void);
bool AppController_IsRemindTimeoutLatched(void);

/** 获取 SOS 键累计触发次数（含重复触发）。 */
uint32_t AppController_GetSosTriggerCount(void);

/** 将状态枚举转为可读字符串，供日志和显示使用。 */
const char *AppController_StateToString(app_state_t state);
