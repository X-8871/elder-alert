#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "RiskEngine.h"
#include "SensorHub.h"

#define APP_CONTROLLER_NO_MOTION_TIMEOUT_MS 30000
#define APP_CONTROLLER_REMIND_CONFIRM_TIMEOUT_MS 15000

typedef enum {
    APP_STATE_NORMAL = 0,
    APP_STATE_REMIND,
    APP_STATE_ALARM,
    APP_STATE_SOS,
} app_state_t;

esp_err_t AppController_Init(void);
esp_err_t AppController_Process(const sensor_hub_data_t *sensor_data, const risk_result_t *risk_result);
esp_err_t AppController_Service(void);
app_state_t AppController_GetState(void);
uint32_t AppController_GetInactiveTimeMs(void);
bool AppController_IsSosLatched(void);
bool AppController_IsRemindTimeoutLatched(void);
uint32_t AppController_GetSosTriggerCount(void);
const char *AppController_StateToString(app_state_t state);
