/**
 * @file BSP_OLED.h
 * @brief SSD1306 128×64 OLED 显示屏驱动封装——通过 I2C 总线通信。
 *
 * 备选显示方案，当前项目主显示使用 TFT(ST7735)。
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#define BSP_OLED_WIDTH           128
#define BSP_OLED_HEIGHT           64
#define BSP_OLED_MAX_LINES         4    /* 最多 4 行文本 */
#define BSP_OLED_LINE_HEIGHT      16    /* 每行 16 像素高（含行间距） */
#define BSP_OLED_MAX_LINE_CHARS   21    /* 每行最多 21 个字符（128/6≈21） */

esp_err_t BSP_OLED_Init(void);
bool BSP_OLED_IsReady(void);

/** 清空整个屏幕 */
esp_err_t BSP_OLED_Clear(void);

/**
 * @brief 将多行文本渲染到帧缓冲区并刷新屏幕。
 * @param lines      文本行数组（每行以 '\0' 结尾）
 * @param line_count 文本行数（最多 4 行）
 */
esp_err_t BSP_OLED_ShowLines(const char *lines[], size_t line_count);
