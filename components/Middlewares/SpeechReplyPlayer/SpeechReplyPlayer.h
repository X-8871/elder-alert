/**
 * @file SpeechReplyPlayer.h
 * @brief 从云端下载 TTS 生成的 WAV 音频，通过 MAX98357A 播放出来。
 *
 * 【学弟必读：这个模块解决什么问题？】
 * 当系统状态发生变化（例如进入 REMIND、进入 ALARM），
 * AppController 会输出一个 event_key（例如 "no_motion_remind"）。
 * SpeechReplyPlayer 负责：
 *   1. 拼出完整 URL → GET /api/voice-prompts/audio?event_key=no_motion_remind
 *   2. 从云端下载 TTS 生成的 WAV 音频文件（HTTP GET，带重试）
 *   3. 解析 WAV 头（获取采样率、声道数等参数）
 *   4. 初始化 MAX98357A 功放
 *   5. 把 PCM 数据写入 I2S TX 通道 → 喇叭发声
 *   6. 播放前后各加 80ms 静音缓冲（减少 I2S 启动/停止时的 POP 杂音）
 *
 * 【硬性约束】
 * - 只支持 16-bit 单声道 PCM WAV（当前云端 MiMo TTS 的输出格式）
 * - 最多重试 3 次，每次间隔 1 秒
 * - 下载失败时由 main.c 回退到 VoicePrompt 本地固定人声
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
