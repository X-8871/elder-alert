/**
 * @file BSP_MAX98357A.h
 * @brief MAX98357A I2S 数字功放 BSP 驱动封装。
 *
 * 当前用于功放 bring-up：初始化 I2S TX，并输出最小 PCM 测试音。
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
#define BSP_MAX98357A_DEFAULT_VOLUME         6000

typedef struct {
    gpio_num_t bclk_gpio;      /* MAX98357A BCLK */
    gpio_num_t ws_gpio;        /* MAX98357A LRC / WS */
    gpio_num_t data_out_gpio;  /* MAX98357A DIN */
    uint32_t sample_rate_hz;
} bsp_max98357a_config_t;

esp_err_t BSP_MAX98357A_Init(const bsp_max98357a_config_t *config);
esp_err_t BSP_MAX98357A_Deinit(void);
esp_err_t BSP_MAX98357A_WriteMonoSamples(const int16_t *samples, size_t sample_count, uint32_t timeout_ms);
esp_err_t BSP_MAX98357A_PlayTone(uint32_t frequency_hz, uint32_t duration_ms, int16_t amplitude);
esp_err_t BSP_MAX98357A_PlaySilence(uint32_t duration_ms);
bool BSP_MAX98357A_IsInitialized(void);
