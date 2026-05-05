#pragma once

#include <stdbool.h>

#include "driver/gpio.h"
#include "esp_err.h"

typedef enum {
    BSP_ALERT_MODE_OFF = 0,
    BSP_ALERT_MODE_NORMAL,
    BSP_ALERT_MODE_ALARM,
    BSP_ALERT_MODE_REMIND,
    BSP_ALERT_MODE_SOS,
} bsp_alert_mode_t;

esp_err_t BSP_Alert_Init(gpio_num_t led_gpio, gpio_num_t buzzer_gpio);
esp_err_t BSP_Alert_SetMode(bsp_alert_mode_t mode);
esp_err_t BSP_Alert_Update(void);
esp_err_t BSP_Alert_SetOutputs(bool led_on, bool buzzer_on);
bsp_alert_mode_t BSP_Alert_GetMode(void);
bool BSP_Alert_IsInitialized(void);
