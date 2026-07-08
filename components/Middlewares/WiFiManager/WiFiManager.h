/*
 * WiFiManager —— Wi-Fi 联网管理模块
 *
 * BLE 配网 + Wi-Fi STA 连接，7 态状态机（事件驱动）：
 *   IDLE → PROVISIONING → CONNECTING → CONNECTED
 *                                   ↘ RECONNECTING → CONNECTED
 *                                   ↘ FAILED
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

/* 联网状态枚举 */
typedef enum {
    WIFI_MANAGER_STATE_IDLE = 0,          /* 空闲：初始化前的默认状态 */
    WIFI_MANAGER_STATE_PROVISIONING,      /* 配网中：BLE 广播等待手机发送 Wi-Fi 凭据 */
    WIFI_MANAGER_STATE_CONNECTING,        /* 连接中：已有凭据，正在通过 esp_wifi_connect() 连路由器 */
    WIFI_MANAGER_STATE_CONNECTED,         /* 已联网：收到 IP_EVENT_STA_GOT_IP，设备具备网络通信能力 */
    WIFI_MANAGER_STATE_RECONNECTING,      /* 重连中：之前连上过但掉线，正在自动重连 */
    WIFI_MANAGER_STATE_DISCONNECTED,      /* 已断开：与路由器断开且不在重连流程中 */
    WIFI_MANAGER_STATE_FAILED,            /* 失败：初始化或流程中发生不可恢复的错误 */
} wifi_manager_state_t;

/*
 * 初始化 WiFiManager。
 * 幂等：重复调用直接返回 ESP_OK。
 */
esp_err_t WiFiManager_Init(void);

/* 清除 NVS 中保存的 Wi-Fi 凭据并重启设备。 */
esp_err_t WiFiManager_ResetProvisioningAndRestart(void);

/* 获取当前联网状态原始枚举值 */
wifi_manager_state_t WiFiManager_GetState(void);

/* 判断设备是否已联网（状态 == CONNECTED）。上层最常用的查询接口。 */
bool WiFiManager_IsConnected(void);

/* 获取当前状态的全称字符串，用于日志输出。如 "CONNECTED"、"PROVISIONING" */
const char *WiFiManager_GetStatusString(void);

/* 获取当前状态的简称，用于 OLED 等小屏显示。如 "OK"、"PROV"、"RETRY" */
const char *WiFiManager_GetStatusShortString(void);

/*
 * 获取当前 IP 地址字符串，如 "192.168.1.103"。
 * 未联网时返回 "0.0.0.0"。
 */
const char *WiFiManager_GetIpString(void);
