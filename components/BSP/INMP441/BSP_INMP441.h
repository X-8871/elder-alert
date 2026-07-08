/**
 * @file BSP_INMP441.h
 * @brief INMP441 I2S 数字 MEMS 麦克风 BSP 驱动封装。
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"

#define BSP_INMP441_DEFAULT_SAMPLE_RATE_HZ 16000U  /* 默认 16kHz 采样率 */
#define BSP_INMP441_DEFAULT_DMA_FRAME_NUM  256U    /* DMA 每帧采样数 */
#define BSP_INMP441_DEFAULT_DMA_DESC_NUM   4U      /* DMA 描述符数量 */

/** 初始化配置 */
typedef struct {
    gpio_num_t bclk_gpio;       /* BCLK 位时钟引脚 */
    gpio_num_t ws_gpio;         /* WS/LRC 左右声道时钟引脚 */
    gpio_num_t data_in_gpio;    /* 麦克风数据输出 → ESP32 数据输入 */
    uint32_t sample_rate_hz;    /* 采样率 (Hz)，常用 16000 */
} bsp_inmp441_config_t;

/** 一帧音频的音量指标 */
typedef struct {
    int32_t peak_abs;       /* 本帧最大绝对幅度 */
    int32_t mean_abs;       /* 本帧平均绝对幅度 */
    size_t sample_count;    /* 本帧采样点数 */
} bsp_inmp441_level_t;

esp_err_t BSP_INMP441_Init(const bsp_inmp441_config_t *config);

/** 释放 I2S RX 通道——与 MAX98357A 共享 BCLK/WS 时需要半双工切换 */
esp_err_t BSP_INMP441_Deinit(void);

/**
 * @brief 读取原始采样数据。
 * @param samples      输出：采样数据缓冲区（调用者分配）
 * @param sample_count 期望读取的采样数
 * @param samples_read 输出：实际读取的采样数
 * @param timeout_ms   超时 (ms)
 */
esp_err_t BSP_INMP441_ReadSamples(int32_t *samples, size_t sample_count,
                                  size_t *samples_read, uint32_t timeout_ms);

/**
 * @brief 读取一帧并计算音量指标。
 */
esp_err_t BSP_INMP441_ReadLevel(bsp_inmp441_level_t *level, uint32_t timeout_ms);

bool BSP_INMP441_IsInitialized(void);
