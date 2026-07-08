/**
 * @file BSP_MAX98357A.h
 * @brief MAX98357A I2S 数字 D 类功放 BSP 驱动封装。
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"

#define BSP_MAX98357A_DEFAULT_SAMPLE_RATE_HZ 16000U
#define BSP_MAX98357A_DEFAULT_DMA_FRAME_NUM  256U
#define BSP_MAX98357A_DEFAULT_DMA_DESC_NUM   4U
#define BSP_MAX98357A_DEFAULT_VOLUME         6000  /* 默认输出幅度（不是音量百分比） */

typedef struct {
    gpio_num_t bclk_gpio;       /* BCLK 位时钟 */
    gpio_num_t ws_gpio;         /* WS/LRC 左右声道时钟 */
    gpio_num_t data_out_gpio;   /* 数据输出 → MAX98357A DIN */
    gpio_num_t sd_gpio;         /* SD 关断引脚，-1 表示未连接 */
    uint32_t sample_rate_hz;    /* 采样率 */
} bsp_max98357a_config_t;

esp_err_t BSP_MAX98357A_Init(const bsp_max98357a_config_t *config);
esp_err_t BSP_MAX98357A_Deinit(void);

/**
 * @brief 将单声道 PCM 采样写入 I2S TX 通道。
 *        实际会复制为双声道（左=右）以满足 I2S 立体声格式。
 * @param samples      16-bit 有符号采样数组
 * @param sample_count 采样数
 * @param timeout_ms   I2S 写入超时
 */
esp_err_t BSP_MAX98357A_WriteMonoSamples(const int16_t *samples, size_t sample_count, uint32_t timeout_ms);

/**
 * @brief 播放一段方波测试音。
 * @param frequency_hz 频率 (Hz)
 * @param duration_ms  持续时长 (ms)
 * @param amplitude    幅度 (0~32767)
 */
esp_err_t BSP_MAX98357A_PlayTone(uint32_t frequency_hz, uint32_t duration_ms, int16_t amplitude);

/** 播放静音（输出全 0 采样） */
esp_err_t BSP_MAX98357A_PlaySilence(uint32_t duration_ms);

bool BSP_MAX98357A_IsInitialized(void);
