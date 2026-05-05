#pragma once

#include <stdbool.h>

#include "AppController.h"
#include "RiskEngine.h"
#include "SensorHub.h"
#include "esp_err.h"

esp_err_t DisplayController_Init(void);
esp_err_t DisplayController_Update(app_state_t app_state,
                                   const sensor_hub_data_t *sensor_data,
                                   const risk_result_t *risk_result);
bool DisplayController_IsEnabled(void);
