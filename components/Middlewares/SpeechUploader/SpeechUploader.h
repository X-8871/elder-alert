/**
 * @file SpeechUploader.h
 * @brief INMP441 短录音上传到云端 ASR 的最小联调工具。
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

/** 录音上传过程阶段，用于驱动不依赖音频数据的界面反馈。 */
typedef enum {
    SPEECH_UPLOADER_PHASE_RECORDING = 0,
    SPEECH_UPLOADER_PHASE_UPLOADING,
} speech_uploader_phase_t;

typedef void (*speech_uploader_phase_callback_t)(speech_uploader_phase_t phase, void *user_ctx);

esp_err_t SpeechUploader_Init(const speech_uploader_config_t *config);
esp_err_t SpeechUploader_Deinit(void);
void SpeechUploader_SetPhaseCallback(speech_uploader_phase_callback_t callback, void *user_ctx);
esp_err_t SpeechUploader_RecordWavAndUpload(uint32_t record_ms);
