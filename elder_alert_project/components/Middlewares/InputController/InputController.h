/**
 * @file InputController.h
 * @brief 输入控制器，管理确认键（GPIO7）、SOS 键（GPIO8）和录音键（GPIO17）的事件检测。
 *
 * 确认键：轮询消抖，支持短按（确认）和长按（配网重置）两种事件。
 * SOS 键：中断触发，一次性事件，用于紧急求助。
 * 录音键：中断触发，一次性事件，用于语音上传测试或后续按键说话。
 */

#pragma once

#include <stdbool.h>

#include "esp_err.h"

/** 初始化两个按键的 GPIO 和中断配置。 */
esp_err_t InputController_Init(void);

/** 检测确认键是否产生了一次短按事件（消抖后）。 */
esp_err_t InputController_GetConfirmEvent(bool *confirmed);

/** 检测确认键是否连续长按超过 8 秒，用于触发配网重置。 */
esp_err_t InputController_GetConfirmLongPressEvent(bool *long_pressed);

/** 检测 SOS 键是否产生了一次按下事件（中断驱动）。 */
esp_err_t InputController_GetSosEvent(bool *sos_triggered);

/** 检测录音键是否产生了一次按下事件（中断驱动）。 */
esp_err_t InputController_GetRecordEvent(bool *record_triggered);
