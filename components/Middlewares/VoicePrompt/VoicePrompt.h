/**
 * @file VoicePrompt.h
 * @brief 固定 PCM 语音提示播放工具。
 */

#pragma once

#include "BSP_MAX98357A.h"
#include "esp_err.h"

esp_err_t VoicePrompt_PlayUploadOk(const bsp_max98357a_config_t *amp_config);
