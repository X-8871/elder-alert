#pragma once

void KEY_Init(void);
int KEY_IsPressed(void);
int KEY_Scan(int *is_pressed);

void KEY_EXTI_Init(void);
int KEY_EXTI_IsPressed(void);
int KEY_EXTI_GetEvent(int *is_pressed);
