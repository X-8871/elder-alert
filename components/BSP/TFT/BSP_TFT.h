/**
 * @file BSP_TFT.h
 * @brief ST7735 1.8寸 128×160 SPI TFT 彩屏 BSP 驱动封装。
 *
 * 使用 ESP-IDF 的 st7789 驱动 + ST7735 专用初始化序列 + gap/mirror/swap_xy 参数补偿。
 * 逻辑分辨率：横屏 160×128，SPI 时钟 10MHz，颜色格式 RGB565。
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"

/* ---- 分辨率 ---- */
#define BSP_TFT_H_RES              160  /* 横屏宽度 */
#define BSP_TFT_V_RES              128  /* 横屏高度 */

/* ---- SPI 配置 ---- */
#define BSP_TFT_SPI_HOST           SPI2_HOST
#define BSP_TFT_PIXEL_CLOCK_HZ     (10 * 1000 * 1000)  /* 10 MHz */
#define BSP_TFT_CMD_BITS           8
#define BSP_TFT_PARAM_BITS         8
#define BSP_TFT_DRAW_BUFFER_LINES  20   /* LVGL 绘制缓冲行数 */

/* ---- 引脚分配 ---- */
#define BSP_TFT_CLK_GPIO           GPIO_NUM_40
#define BSP_TFT_MOSI_GPIO          GPIO_NUM_41
#define BSP_TFT_RST_GPIO           GPIO_NUM_42
#define BSP_TFT_DC_GPIO            GPIO_NUM_39  /* Data/Command 选择 */
#define BSP_TFT_CS_GPIO            GPIO_NUM_11  /* SPI 片选 */
#define BSP_TFT_BL_GPIO            GPIO_NUM_47  /* 背光控制 */

/** 颜色传输完成的回调 */
typedef struct {
    esp_lcd_panel_io_color_trans_done_cb_t on_color_trans_done;
    void *user_ctx;  /* 回调的用户上下文参数 */
} bsp_tft_callbacks_t;

esp_err_t BSP_TFT_Init(const bsp_tft_callbacks_t *callbacks);
esp_err_t BSP_TFT_RegisterCallbacks(const bsp_tft_callbacks_t *callbacks);
bool BSP_TFT_IsReady(void);

/** 获取 LCD 面板句柄——LVGL 需要用它来注册显示驱动 */
esp_lcd_panel_handle_t BSP_TFT_GetPanelHandle(void);

spi_host_device_t BSP_TFT_GetSpiHost(void);
uint16_t BSP_TFT_GetWidth(void);
uint16_t BSP_TFT_GetHeight(void);

/** 背光开关控制 */
esp_err_t BSP_TFT_SetBacklight(bool on);
