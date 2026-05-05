#include "DisplayController.h"

#include <stdio.h>
#include <string.h>

#include "BSP_OLED.h"
#include "WiFiManager.h"
#include "esp_log.h"

static const char *TAG = "DisplayController";

static bool s_enabled = false;

static const char *risk_level_short_string(risk_level_t level)
{
    switch (level) {
    case RISK_LEVEL_NORMAL:
        return "NORMAL";
    case RISK_LEVEL_REMIND:
        return "REMIND";
    case RISK_LEVEL_WARNING:
        return "WARN";
    case RISK_LEVEL_EMERGENCY:
        return "EMERG";
    default:
        return "UNKNOWN";
    }
}

esp_err_t DisplayController_Init(void)
{
    esp_err_t ret = BSP_OLED_Init();
    if (ret != ESP_OK) {
        s_enabled = false;
        ESP_LOGW(TAG, "OLED unavailable, display disabled: %s", esp_err_to_name(ret));
        return ret;
    }

    s_enabled = true;
    return ESP_OK;
}

bool DisplayController_IsEnabled(void)
{
    return s_enabled;
}

esp_err_t DisplayController_Update(app_state_t app_state,
                                   const sensor_hub_data_t *sensor_data,
                                   const risk_result_t *risk_result)
{
    if (!s_enabled) {
        return ESP_OK;
    }
    if (sensor_data == NULL || risk_result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char line0[22] = {0};
    char line1[22] = {0};
    char line2[22] = {0};
    char line3[22] = {0};
    const char *lines[BSP_OLED_MAX_LINES] = {line0, line1, line2, line3};

    snprintf(line0, sizeof(line0), "ST:%s", AppController_StateToString(app_state));
    snprintf(line1, sizeof(line1), "T:%.1fC H:%.0f%%", sensor_data->aht_temperature, sensor_data->humidity);
    snprintf(line2, sizeof(line2), "P:%.1fK L:%u", sensor_data->pressure / 1000.0f, (unsigned int)sensor_data->lux);
    snprintf(line3,
             sizeof(line3),
             "R:%s W:%s",
             risk_level_short_string(risk_result->level),
             WiFiManager_GetStatusShortString());

    return BSP_OLED_ShowLines(lines, BSP_OLED_MAX_LINES);
}
