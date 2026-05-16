#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"

#define BSP_TFT_H_RES              128
#define BSP_TFT_V_RES              160
#define BSP_TFT_SPI_HOST           SPI2_HOST
#define BSP_TFT_PIXEL_CLOCK_HZ     (20 * 1000 * 1000)
#define BSP_TFT_CMD_BITS           8
#define BSP_TFT_PARAM_BITS         8
#define BSP_TFT_DRAW_BUFFER_LINES  20

#define BSP_TFT_CLK_GPIO           GPIO_NUM_40
#define BSP_TFT_MOSI_GPIO          GPIO_NUM_41
#define BSP_TFT_RST_GPIO           GPIO_NUM_42
#define BSP_TFT_DC_GPIO            GPIO_NUM_39
#define BSP_TFT_CS_GPIO            GPIO_NUM_38
#define BSP_TFT_BL_GPIO            GPIO_NUM_47

typedef struct {
    esp_lcd_panel_io_color_trans_done_cb_t on_color_trans_done;
    void *user_ctx;
} bsp_tft_callbacks_t;

esp_err_t BSP_TFT_Init(const bsp_tft_callbacks_t *callbacks);
esp_err_t BSP_TFT_RegisterCallbacks(const bsp_tft_callbacks_t *callbacks);
bool BSP_TFT_IsReady(void);
esp_lcd_panel_handle_t BSP_TFT_GetPanelHandle(void);
spi_host_device_t BSP_TFT_GetSpiHost(void);
uint16_t BSP_TFT_GetWidth(void);
uint16_t BSP_TFT_GetHeight(void);
esp_err_t BSP_TFT_SetBacklight(bool on);
