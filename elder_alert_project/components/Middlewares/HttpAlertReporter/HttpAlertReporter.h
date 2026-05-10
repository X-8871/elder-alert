/**
 * @file HttpAlertReporter.h
 * @brief HTTP 告警上报器，通过 HTTP POST 将异常事件和周期遥测数据上报至云端服务。
 *
 * 上报策略：
 *   - 事件上报（EVENT）：状态变化、风险原因变化、SOS 新触发时立即上报
 *   - 遥测上报（TELEMETRY）：固定间隔周期上报当前传感器数据
 *   - 设备标识使用 WiFi STA MAC 地址
 */

#pragma once

#include "AppController.h"
#include "RiskEngine.h"
#include "SensorHub.h"
#include "esp_err.h"

esp_err_t HttpAlertReporter_Init(void);

/** 根据当前状态判断是否需要上报，构建 JSON 并发送 HTTP POST。 */
esp_err_t HttpAlertReporter_Process(app_state_t state,
                                    const sensor_hub_data_t *sensor_data,
                                    const risk_result_t *risk_result);
