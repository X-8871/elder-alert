/**
 * @file BSP_MQ2.h
 * @brief MQ2 烟雾/可燃气体传感器驱动封装——通过 ADC（模数转换器）采集模拟信号。
 *
 * 【学弟必读：MQ2 传感器原理】
 * MQ2 是一个半导体气敏传感器，能检测烟雾、液化气、甲烷等可燃气体。
 * - 输出模拟电压：气体浓度越高 → 传感器电导率越大 → 输出电压越高。
 * - ESP32 没有电压表，只能用 ADC 把模拟电压转换成数字量（0~4095）。
 * - ADC 原始值和实际电压之间有偏差，需要"校准"（calibration）来修正。
 *
 * 【校准是什么？】
 * ESP32 ADC 的原始值 raw 和实际电压 mV 不是一一对应的（有非线性误差）。
 * 本驱动支持两种自动校准方案：
 * - 曲线拟合（curve fitting）：更精确，需要芯片支持。
 * - 线性拟合（line fitting）：较简单，兼容性更好。
 * 校准成功时 voltage_mv 有实际电压值；不成功时 voltage_mv = -1。
 */

#pragma once

#include <stdbool.h>
#include "driver/gpio.h"  /* gpio_num_t */
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
