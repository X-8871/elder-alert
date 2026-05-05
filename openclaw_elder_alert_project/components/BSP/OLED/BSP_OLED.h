#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#define BSP_OLED_WIDTH           128
#define BSP_OLED_HEIGHT           64
#define BSP_OLED_MAX_LINES         4
#define BSP_OLED_LINE_HEIGHT      16
#define BSP_OLED_MAX_LINE_CHARS   21

esp_err_t BSP_OLED_Init(void);
bool BSP_OLED_IsReady(void);
esp_err_t BSP_OLED_Clear(void);
esp_err_t BSP_OLED_ShowLines(const char *lines[], size_t line_count);
