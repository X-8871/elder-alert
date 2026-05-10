/**
 * @file SpeechUploader.h
 * @brief INMP441 短录音上传到云端 ASR 的最小 bring-up 工具。
 *
 * 默认不接入业务状态机；仅在 main 中打开测试宏时使用，避免改变现有按键和报警语义。
 */

#pragma once

#include <stdint.h>

#include "BSP_INMP441.h"
#include "driver/gpio.h"
#include "esp_err.h"

#define SPEECH_UPLOADER_DEFAULT_RECORD_MS 3000U
#define SPEECH_UPLOADER_MAX_RECORD_MS     5000U

typedef struct {
    gpio_num_t bclk_gpio;
    gpio_num_t ws_gpio;
    gpio_num_t data_in_gpio;
    uint32_t sample_rate_hz;
} speech_uploader_config_t;

esp_err_t SpeechUploader_Init(const speech_uploader_config_t *config);
esp_err_t SpeechUploader_RecordWavAndUpload(uint32_t record_ms);
