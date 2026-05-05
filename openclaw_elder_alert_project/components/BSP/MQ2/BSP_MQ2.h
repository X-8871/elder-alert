#pragma once

#include <stdbool.h>
#include "driver/gpio.h"
#include "esp_err.h"

typedef struct {
    int raw;
    int voltage_mv;
} bsp_mq2_reading_t;

esp_err_t BSP_MQ2_Init(gpio_num_t analog_gpio);
esp_err_t BSP_MQ2_Read(bsp_mq2_reading_t *reading);
bool BSP_MQ2_IsInitialized(void);
