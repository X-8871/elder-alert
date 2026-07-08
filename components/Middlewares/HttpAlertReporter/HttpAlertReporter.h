/**
 * @file HttpAlertReporter.h
 * @brief HTTP 告警上报器，通过 HTTP POST 将异常事件和周期遥测数据上报至云端服务。
 *
 * 上报策略：
 *   - 事件上报（EVENT）：状态变化、风险原因变化、SOS 新触发时立即上报
 *   - 遥测上报（TELEMETRY）：固定间隔周期上报当前传感器数据
 *   - 设备标识使用 Wi-Fi STA MAC 地址
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "AppController.h"
#include "RiskEngine.h"
#include "SensorHub.h"
#include "esp_err.h"

/* ---- Agent 设备命令类型 ---- */
#define HTTP_ALERT_REPORTER_MAX_COMMANDS 4U
#define HTTP_ALERT_REPORTER_MAX_CMD_TYPE_LEN 32U
#define HTTP_ALERT_REPORTER_MAX_CMD_MSG_LEN 128U
#define HTTP_ALERT_REPORTER_MAX_CMD_URL_LEN 128U

typedef enum {
    DEVICE_CMD_NONE = 0,
    DEVICE_CMD_CONFIRM_ALERT,      /* 远程确认/关闭提醒报警 */
    DEVICE_CMD_SHOW_SCREEN_MESSAGE, /* TFT 屏幕显示消息 */
    DEVICE_CMD_BEEP_ONCE,           /* 蜂鸣器短响一声 */
    DEVICE_CMD_PLAY_TTS,            /* 播放 TTS 语音 */
    DEVICE_CMD_SET_RUN_MODE,        /* 运行时切换 DEMO/REAL 模式 */
} device_cmd_type_t;

typedef struct {
    device_cmd_type_t type;
    char message[HTTP_ALERT_REPORTER_MAX_CMD_MSG_LEN];   /* show_screen_message 的文字 */
    char url[HTTP_ALERT_REPORTER_MAX_CMD_URL_LEN];         /* play_tts 的音频 URL */
    int duration;                                           /* show_screen_message 的持续时间(秒) */
    int run_mode;                                           /* set_run_mode: 0=DEMO, 1=REAL */
} device_command_t;

esp_err_t HttpAlertReporter_Init(void);

/** 根据当前状态判断是否需要上报，构建 JSON 并发送 HTTP POST。
 *  上报后会自动从响应中解析服务器下发的设备命令并缓存。 */
esp_err_t HttpAlertReporter_Process(app_state_t state,
                                    const sensor_hub_data_t *sensor_data,
                                    const risk_result_t *risk_result);

/** 取出服务器下发的设备命令（调用即消费）。返回命令数量，0 表示无命令。 */
uint32_t HttpAlertReporter_TakePendingCommands(device_command_t *out_commands,
                                                uint32_t max_count);
