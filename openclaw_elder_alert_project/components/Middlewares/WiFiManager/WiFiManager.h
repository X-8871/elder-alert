/*
 * WiFiManager —— Wi-Fi 联网管理模块
 *
 * 本模块封装了 ESP32 从"零"到"拿到 IP"的完整联网链路：
 *   BLE 配网 → NVS 保存凭据 → Wi-Fi STA 连接路由器 → DHCP 获取 IP
 *
 * 设计核心是 7 态状态机（事件驱动）：
 *   IDLE → PROVISIONING → CONNECTING → CONNECTED
 *                                   ↘ RECONNECTING → CONNECTED
 *                                   ↘ FAILED
 *
 * 上层模块（如 AppController）只需调用 WiFiManager_IsConnected() 判断网络是否可用，
 * 无需关心配网、连 Wi-Fi、DHCP、重连等底层细节。
 *
 * 安全说明：
 *   - BLE 配网使用 PoP（Proof of Possession）做身份校验，防止陌生人乱配网
 *   - 凭据通过 NVS 持久化存储，断电重启后自动重连，无需重新配网
 *   - WiFiManager_ResetProvisioningAndRestart() 会清除 NVS 中的凭据并重启
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

/*
 * 联网状态枚举 —— 7 个平级状态，设备同一时刻只处于其中一个
 *
 * 典型流转路径：
 *   上电 → IDLE → PROVISIONING（首次，走 BLE 配网）
 *        → IDLE → CONNECTING（已配过网，直接连）
 *        → CONNECTING → CONNECTED（拿到 IP）
 *        → CONNECTED → RECONNECTING（断线自动重连）
 *        → RECONNECTING → CONNECTED（重连成功）
 */
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
 * 初始化 WiFiManager，完成从 NVS 到事件注册的 6 层初始化，
 * 然后根据"是否已配过网"决定走 BLE 配网还是直接连 Wi-Fi。
 * 幂等：重复调用直接返回 ESP_OK。
 */
esp_err_t WiFiManager_Init(void);

/*
 * 清除 NVS 中保存的 Wi-Fi 凭据并重启设备。
 * 重启后 network_prov_mgr_is_wifi_provisioned() 返回 false，
 * 设备将重新进入 BLE 配网流程。
 * 典型触发场景：用户长按确认键 8 秒。
 */
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
