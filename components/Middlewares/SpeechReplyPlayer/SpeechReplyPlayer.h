/**
 * @file SpeechReplyPlayer.h
 * @brief 从云端下载 TTS 生成的 WAV 音频，通过 MAX98357A 播放出来。
 *
 * AppController 输出 event_key → 拼接 URL → HTTP GET 下载 WAV → 解析 WAV 头
 * → 初始化 MAX98357A → I2S 写入 PCM → 播放前后各加 80ms 静音缓冲减少 POP 杂音。
 *
 * 只支持 16-bit 单声道 PCM WAV，最多重试 3 次，下载失败时由 main.c 回退到 VoicePrompt。
 */

#pragma once

#include <stdint.h>
#include "BSP_MAX98357A.h"
#include "esp_err.h"

/**
 * @brief 下载并播放最新的 AI 回复音频。
 *        URL 固定为 /api/speech/reply-audio
 *        用于"录音上传后 → AI 生成回复 → TTS 转语音 → 播放"的场景。
 */
esp_err_t SpeechReplyPlayer_PlayLatest(const bsp_max98357a_config_t *base_amp_config);

/**
 * @brief 根据 event_key 下载并播放状态播报音频。
 *        用于系统状态变化时的语音播报（如进入 REMIND/ALARM）。
 *        event_key 示例："no_motion_remind", "mq2_danger_alarm", "manual_sos" 等
 */
esp_err_t SpeechReplyPlayer_PlayEventKey(const bsp_max98357a_config_t *base_amp_config,
                                         const char *event_key);
