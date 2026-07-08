/**
 * @file BSP_MQ2.h
 * @brief MQ2 烟雾/可燃气体传感器驱动封装——通过 ADC（模数转换器）采集模拟信号。
 */

#pragma once

#include <stdbool.h>
#include "driver/gpio.h"
#include "esp_err.h"

/** MQ2 单次读数的结果。 */
typedef struct {
    int raw;           /**< ADC 原始值 (0~4095)，越大表示气体浓度越高 */
    int voltage_mv;    /**< 校准后的电压 (mV)，校准不可用时为 -1 */
} bsp_mq2_reading_t;

/**
 * @brief 初始化 MQ2 传感器——配置 ADC1 并创建校准句柄。
 * @param analog_gpio 模拟输入对应的 GPIO（本项目使用 GPIO1 → ADC_CHANNEL_0）
 */
esp_err_t BSP_MQ2_Init(gpio_num_t analog_gpio);

/**
 * @brief 读取一次 ADC 值（含校准）。
 * @param reading 输出：raw 和 voltage_mv
 */
esp_err_t BSP_MQ2_Read(bsp_mq2_reading_t *reading);

bool BSP_MQ2_IsInitialized(void);
