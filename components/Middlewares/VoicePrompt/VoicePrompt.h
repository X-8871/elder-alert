/**
 * @file VoicePrompt.h
 * @brief 本地固定 PCM 语音提示播放工具。
 *
 * 【学弟必读：为什么需要这个模块？】
 * 云端 TTS 生成的语音虽然自然好听，但依赖 Wi-Fi 和服务器。
 * 如果网络不通、服务器挂了、或者 TTS API 出问题——
 * SpeechReplyPlayer 就会失败，这时需要 VoicePrompt 兜底。
 *
 * 当前内置的固定语音：
 * - "我听到了"（16kHz/16-bit/mono PCM）—— 用于录音上传成功后的确认
 *
 * PCM 数据是离线用 Windows 的"Microsoft Huihui Desktop"语音合成工具生成的，
 * 直接以二进制数组形式编译进固件（会占用固件体积）。
 */

#pragma once

#include "BSP_MAX98357A.h"
#include "esp_err.h"

/**
 * @brief 播放"我听到了"提示音。
 *        录音上传成功后的确认反馈——让用户知道系统收到了他的语音。
 */
esp_err_t VoicePrompt_PlayUploadOk(const bsp_max98357a_config_t *amp_config);
