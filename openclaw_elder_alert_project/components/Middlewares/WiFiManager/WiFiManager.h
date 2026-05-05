#pragma once

#include <stdbool.h>

#include "esp_err.h"

typedef enum {
    WIFI_MANAGER_STATE_IDLE = 0,
    WIFI_MANAGER_STATE_PROVISIONING,
    WIFI_MANAGER_STATE_CONNECTING,
    WIFI_MANAGER_STATE_CONNECTED,
    WIFI_MANAGER_STATE_RECONNECTING,
    WIFI_MANAGER_STATE_DISCONNECTED,
    WIFI_MANAGER_STATE_FAILED,
} wifi_manager_state_t;

esp_err_t WiFiManager_Init(void);
esp_err_t WiFiManager_ResetProvisioningAndRestart(void);
wifi_manager_state_t WiFiManager_GetState(void);
bool WiFiManager_IsConnected(void);
const char *WiFiManager_GetStatusString(void);
const char *WiFiManager_GetStatusShortString(void);
const char *WiFiManager_GetIpString(void);
