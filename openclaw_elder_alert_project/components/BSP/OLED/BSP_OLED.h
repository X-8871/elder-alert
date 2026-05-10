/**
 * @file BSP_OLED.h
 * @brief SSD1306 128×64 OLED 显示屏驱动封装，通过 I2C 总线通信。
 *
 * 使用内置 5×7 字模点阵渲染 ASCII 文本，支持 4 行显示，每行最多 21 字符。
 * 采用页寻址帧缓冲区，整屏刷新。
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#define BSP_OLED_WIDTH           128  /* 屏幕宽度 (像素) */
#define BSP_OLED_HEIGHT           64  /* 屏幕高度 (像素) */
#define BSP_OLED_MAX_LINES         4  /* 最大显示行数 */
#define BSP_OLED_LINE_HEIGHT      16  /* 每行高度 (像素) */
#define BSP_OLED_MAX_LINE_CHARS   21  /* 每行最大字符数 */

esp_err_t BSP_OLED_Init(void);
bool BSP_OLED_IsReady(void);
esp_err_t BSP_OLED_Clear(void);

/** 将多行文本渲染到帧缓冲区并刷新到屏幕。 */
esp_err_t BSP_OLED_ShowLines(const char *lines[], size_t line_count);
