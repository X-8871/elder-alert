/**
 * @file KEY.h
 * @brief 按键驱动——管理板载两个按键：确认键(GPIO7)、SOS键(GPIO8)。
 */

#pragma once

/* ---- 确认键（GPIO7，轮询方式）---- */

/** 初始化确认键 GPIO，配置为上拉输入（默认高电平，按下为低电平） */
void KEY_Init(void);

/** 直接读取确认键当前状态：按下返回 1，未按下返回 0 */
int KEY_IsPressed(void);

/**
 * @brief 轮询确认键——带消抖，只在稳定状态发生变化时返回 1。
 *        应在主循环中持续调用。
 * @param is_pressed 输出参数：1 表示当前是按下状态，0 表示松开状态
 * @return 1 表示本次发生了状态变化，0 表示无变化
 */
int KEY_Scan(int *is_pressed);

/* ---- SOS 键（GPIO8，中断方式）---- */

/**
 * @brief 初始化 SOS 键的中断（GPIO8 双沿触发）。
 *        同时安装 GPIO ISR 服务（全局只需一次），绑定引脚的中断回调。
 */
void KEY_EXTI_Init(void);

/** 直接读取 SOS 键当前状态 */
int KEY_EXTI_IsPressed(void);

/**
 * @brief 获取 SOS 键中断事件——带消抖。
 *        应在主循环中持续调用以消费中断产生的事件。
 * @param is_pressed 输出参数：1 表示按下，0 表示松开
 * @return 1 表示发生了有效按键事件，0 表示无事件
 */
int KEY_EXTI_GetEvent(int *is_pressed);
