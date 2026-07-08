/**
 * @file BSP_OLED.h
 * @brief SSD1306 128×64 OLED 显示屏驱动封装——通过 I2C 总线通信。
 *
 * 【学弟必读：OLED 显示原理】
 * SSD1306 是一个 128×64 像素的单色 OLED 控制器。每个像素只有两种状态：亮(1) 或灭(0)。
 * 内部使用"页寻址"方式组织帧缓冲区：把 64 行分成 8 个"页"（每页 8 行），
 * 每个字节的 8 个 bit 对应一列中 8 个像素的亮灭。
 *
 * 缓冲区大小 = 128(宽) × 64(高) / 8(bits/byte) = 1024 字节。
 *
 * 【字模是怎么渲染的？】
 * 本驱动内置了一份 5×7 ASCII 字模表（数字、大写字母和少量符号）。
 * 每个字符用一个 7 字节的数组表示——每字节代表字符的一行（5 有效位 + 3 填充位）。
 * draw_char() 函数逐行取出字模数据，用 set_pixel() 写入帧缓冲区对应位置。
 *
 * 注意：这个 OLED 驱动是备选显示方案，当前项目主显示使用 TFT(ST7735)。
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
