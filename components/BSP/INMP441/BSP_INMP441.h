/**
 * @file BSP_INMP441.h
 * @brief INMP441 I2S 数字 MEMS 麦克风 BSP 驱动封装。
 *
 * 【学弟必读：I2S 是什么？】
 * I2S (Inter-IC Sound) 是专门用于数字音频传输的串行总线协议。
 * 它用 3 根线传输立体声音频数据：
 * - **BCLK** (Bit Clock)：位时钟，串行数据的时钟基准
 * - **WS/LRC** (Word Select / Left-Right Clock)：左右声道选择，低=左声道，高=右声道
 * - **DIN/DOUT**：数据线，每个 BCLK 脉冲传 1 bit
 *
 * 【INMP441 特性】
 * - 数字 MEMS 麦克风，直接输出 I2S 数字信号（不需要外部 ADC）
 * - 24-bit 精度，但数据放在 32-bit slot 的高位
 * - L/R 引脚接 GND 时输出左声道数据
 * - 本项目采样率设为 16000 Hz（足够语音识别用，不需要 CD 音质 44100 Hz）
 *
 * 【为什么采集后要右移 8 位？】
 * INMP441 输出 24-bit 有效数据，但填充在 32-bit I2S slot 的高 24 位。
 * 右移 8 位是为了把 24-bit 有效数据对齐到 32-bit int 的低位，方便计算幅度。
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

/** 一帧音频的音量指标——用于快速判断有没有人在说话 */
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
 * @brief 读取一帧并计算音量指标——用于串口调试时观察麦克风是否工作。
 */
esp_err_t BSP_INMP441_ReadLevel(bsp_inmp441_level_t *level, uint32_t timeout_ms);

bool BSP_INMP441_IsInitialized(void);
