#pragma once

#include <stdbool.h>

#include "esp_err.h"

/* 初始化两个按键：
 * GPIO7 为确认键，
 * GPIO17 为 SOS 键。 */
esp_err_t InputController_Init(void);

/* 检测确认键是否产生了一次按下事件。 */
esp_err_t InputController_GetConfirmEvent(bool *confirmed);

/* 检测确认键是否连续长按，用于触发配网重置。 */
esp_err_t InputController_GetConfirmLongPressEvent(bool *long_pressed);

/* 检测 SOS 键是否产生了一次按下事件。 */
esp_err_t InputController_GetSosEvent(bool *sos_triggered);
