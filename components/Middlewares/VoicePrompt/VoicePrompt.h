/**
 * @file VoicePrompt.h
 * @brief 本地固定 PCM 语音提示播放工具。
 *
 * 云端 TTS 失败时的兜底方案，当前内置"我听到了"(16kHz/16-bit/mono PCM)。
 */

#pragma once

#include "BSP_MAX98357A.h"
#include "esp_err.h"

/**
 * @brief 播放“我听到了”提示音。
 */
esp_err_t VoicePrompt_PlayUploadOk(const bsp_max98357a_config_t *amp_config);
