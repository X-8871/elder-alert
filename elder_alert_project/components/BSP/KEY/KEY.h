/**
 * @file KEY.h
 * @brief 按键驱动，提供确认键、SOS 键和录音键检测。
 */

#pragma once

void KEY_Init(void);
int KEY_IsPressed(void);

/** 轮询消抖检测，状态变化时返回 1 并通过 is_pressed 输出当前电平。 */
int KEY_Scan(int *is_pressed);

void KEY_EXTI_Init(void);
int KEY_EXTI_IsPressed(void);

/** SOS 键中断 + 消抖检测，事件产生时返回 1 并通过 is_pressed 输出按下/松开。 */
int KEY_EXTI_GetEvent(int *is_pressed);

int KEY_RECORD_IsPressed(void);

/** 录音键中断 + 消抖检测，事件产生时返回 1 并通过 is_pressed 输出按下/松开。 */
int KEY_RECORD_GetEvent(int *is_pressed);
