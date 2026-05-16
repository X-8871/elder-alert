/**
 * @file SpeechReplyPlayer.h
 * @brief 下载最新云端 TTS WAV 回复，并通过 MAX98357A 播放。
 */

#pragma once

#include "BSP_MAX98357A.h"
#include "esp_err.h"

esp_err_t SpeechReplyPlayer_PlayLatest(const bsp_max98357a_config_t *base_amp_config);
