/**
 * @file BSP_MQ2.h
 * @brief MQ2 烟雾/可燃气体传感器驱动封装，通过 ADC 采集模拟信号。
 *
 * 支持 ADC 校准（curve fitting 或 line fitting），校准可用时输出 mV 电压。
 */

#pragma once

#include <stdbool.h>
#include "driver/gpio.h"
#include "esp_err.h"

/** MQ2 单次读数结果。 */
typedef struct {
    int raw;           /* ADC 原始值 */
    int voltage_mv;    /* 校准后电压 (mV)，校准不可用时为 -1 */
} bsp_mq2_reading_t;

esp_err_t BSP_MQ2_Init(gpio_num_t analog_gpio);
esp_err_t BSP_MQ2_Read(bsp_mq2_reading_t *reading);
bool BSP_MQ2_IsInitialized(void);
