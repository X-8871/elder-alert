/**
 * @file RainMakerReporter.h
 * @brief ESP RainMaker 云端上报器，通过 MQTT 将传感器数据和告警事件同步至 RainMaker 平台。
 *
 * 注册的 RainMaker 参数：温度、湿度、光照度、MQ2 原始值、人体活动、
 * 应用状态、风险等级、风险原因。异常事件通过 esp_rmaker_raise_alert 推送通知。
 * 支持 Simple Time Series 时序数据上报（依赖 SNTP 时间同步）。
 */

#pragma once

#include "AppController.h"
#include "RiskEngine.h"
#include "SensorHub.h"
#include "esp_err.h"

esp_err_t RainMakerReporter_Init(void);

/** 更新 RainMaker 参数并按策略上报事件/遥测数据。 */
esp_err_t RainMakerReporter_Process(app_state_t state,
                                    const sensor_hub_data_t *sensor_data,
                                    const risk_result_t *risk_result);
