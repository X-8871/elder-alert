#include "BSP_TFT.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_log.h"

static const char *TAG = "BSP_TFT";

static esp_lcd_panel_io_handle_t s_io = NULL;
static esp_lcd_panel_handle_t s_panel = NULL;
static bool s_initialized = false;
static bool s_backlight_on = false;

esp_err_t BSP_TFT_RegisterCallbacks(const bsp_tft_callbacks_t *callbacks)
{
    if (s_io == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    const esp_lcd_panel_io_callbacks_t io_callbacks = {
        .on_color_trans_done = callbacks ? callbacks->on_color_trans_done : NULL,
    };

    return esp_lcd_panel_io_register_event_callbacks(s_io, &io_callbacks, callbacks ? callbacks->user_ctx : NULL);
}

static esp_err_t init_backlight_gpio(void)
{
    const gpio_config_t bk_gpio_config = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << BSP_TFT_BL_GPIO,
    };

    esp_err_t ret = gpio_config(&bk_gpio_config);
    if (ret != ESP_OK) {
        return ret;
    }

    return BSP_TFT_SetBacklight(false);
}

esp_err_t BSP_TFT_Init(const bsp_tft_callbacks_t *callbacks)
{
    if (s_initialized) {
        return ESP_OK;
    }

    esp_err_t ret = init_backlight_gpio();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "backlight gpio init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    const spi_bus_config_t bus_config = {
        .sclk_io_num = BSP_TFT_CLK_GPIO,
        .mosi_io_num = BSP_TFT_MOSI_GPIO,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = BSP_TFT_H_RES * BSP_TFT_DRAW_BUFFER_LINES * sizeof(uint16_t),
    };

    ret = spi_bus_initialize(BSP_TFT_SPI_HOST, &bus_config, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "spi bus init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    const esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = BSP_TFT_DC_GPIO,
        .cs_gpio_num = BSP_TFT_CS_GPIO,
        .pclk_hz = BSP_TFT_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = BSP_TFT_CMD_BITS,
        .lcd_param_bits = BSP_TFT_PARAM_BITS,
        .spi_mode = 0,
        .trans_queue_depth = 10,
        .on_color_trans_done = callbacks ? callbacks->on_color_trans_done : NULL,
        .user_ctx = callbacks ? callbacks->user_ctx : NULL,
    };

    ret = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BSP_TFT_SPI_HOST, &io_config, &s_io);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "panel io init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = BSP_TFT_RST_GPIO,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };

    ret = esp_lcd_new_panel_st7789(s_io, &panel_config, &s_panel);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "panel driver init failed: %s", esp_err_to_name(ret));
        esp_lcd_panel_io_del(s_io);
        s_io = NULL;
        return ret;
    }

    ret = esp_lcd_panel_reset(s_panel);
    if (ret == ESP_OK) {
        ret = esp_lcd_panel_init(s_panel);
    }
    if (ret == ESP_OK) {
        ret = esp_lcd_panel_invert_color(s_panel, true);
    }
    if (ret == ESP_OK) {
        ret = esp_lcd_panel_mirror(s_panel, true, false);
    }
    if (ret == ESP_OK) {
        ret = esp_lcd_panel_set_gap(s_panel, 0, 0);
    }
    if (ret == ESP_OK) {
        ret = esp_lcd_panel_disp_on_off(s_panel, true);
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "panel startup failed: %s", esp_err_to_name(ret));
        esp_lcd_panel_del(s_panel);
        esp_lcd_panel_io_del(s_io);
        s_panel = NULL;
        s_io = NULL;
        return ret;
    }

    ret = BSP_TFT_SetBacklight(true);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "backlight enable failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_initialized = true;
    ESP_LOGI(TAG,
             "ST7735 TFT ready: %ux%u clk=%d mosi=%d dc=%d cs=%d rst=%d bl=%d",
             BSP_TFT_H_RES,
             BSP_TFT_V_RES,
             BSP_TFT_CLK_GPIO,
             BSP_TFT_MOSI_GPIO,
             BSP_TFT_DC_GPIO,
             BSP_TFT_CS_GPIO,
             BSP_TFT_RST_GPIO,
             BSP_TFT_BL_GPIO);
    ESP_LOGW(TAG, "If the panel is shifted or cropped, adjust ST7735 gap/orientation for your module variant");
    return ESP_OK;
}

bool BSP_TFT_IsReady(void)
{
    return s_initialized;
}

esp_lcd_panel_handle_t BSP_TFT_GetPanelHandle(void)
{
    return s_panel;
}

spi_host_device_t BSP_TFT_GetSpiHost(void)
{
    return BSP_TFT_SPI_HOST;
}

uint16_t BSP_TFT_GetWidth(void)
{
    return BSP_TFT_H_RES;
}

uint16_t BSP_TFT_GetHeight(void)
{
    return BSP_TFT_V_RES;
}

esp_err_t BSP_TFT_SetBacklight(bool on)
{
    esp_err_t ret = gpio_set_level(BSP_TFT_BL_GPIO, on ? 1 : 0);
    if (ret == ESP_OK) {
        s_backlight_on = on;
    }
    return ret;
}
