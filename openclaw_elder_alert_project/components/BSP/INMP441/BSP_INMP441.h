/**
 * @file BSP_INMP441.h
 * @brief INMP441 I2S 数字麦克风 BSP 驱动封装。
 *
 * 当前用于麦克风 bring-up：初始化 I2S RX、读取原始采样，并计算一帧音量指标。
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"

#define BSP_INMP441_DEFAULT_SAMPLE_RATE_HZ 16000U
#define BSP_INMP441_DEFAULT_DMA_FRAME_NUM  256U
#define BSP_INMP441_DEFAULT_DMA_DESC_NUM   4U

typedef struct {
    gpio_num_t bclk_gpio;       /* INMP441 SCK / I2S BCLK */
    gpio_num_t ws_gpio;         /* INMP441 WS / I2S LRCLK */
    gpio_num_t data_in_gpio;    /* INMP441 SD / I2S data out to ESP32 input */
    uint32_t sample_rate_hz;
} bsp_inmp441_config_t;

typedef struct {
    int32_t peak_abs;       /* 本帧最大绝对幅度，24-bit 采样右移到低位后计算 */
    int32_t mean_abs;       /* 本帧平均绝对幅度，用于串口观察音量变化 */
    size_t sample_count;
} bsp_inmp441_level_t;

esp_err_t BSP_INMP441_Init(const bsp_inmp441_config_t *config);
esp_err_t BSP_INMP441_ReadSamples(int32_t *samples, size_t sample_count, size_t *samples_read, uint32_t timeout_ms);
esp_err_t BSP_INMP441_ReadLevel(bsp_inmp441_level_t *level, uint32_t timeout_ms);
bool BSP_INMP441_IsInitialized(void);

