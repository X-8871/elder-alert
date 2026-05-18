#include "BSP_TFT.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_commands.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_log.h"

static const char *TAG = "BSP_TFT";

#define BSP_TFT_SPI_TRANS_QUEUE_DEPTH  10
#define ST7735_COLMOD_RGB565           0x05
#define ST7735_X_GAP                   2
#define ST7735_Y_GAP                   3
#define ST7735_SWAP_XY                 true
#define ST7735_MIRROR_X                true
#define ST7735_MIRROR_Y                false
#define ST7735_INVERT_COLOR            false

#define ST7735_CMD_FRMCTR1             0xB1
#define ST7735_CMD_FRMCTR2             0xB2
#define ST7735_CMD_FRMCTR3             0xB3
#define ST7735_CMD_INVCTR              0xB4
#define ST7735_CMD_PWCTR1              0xC0
#define ST7735_CMD_PWCTR2              0xC1
#define ST7735_CMD_PWCTR3              0xC2
#define ST7735_CMD_PWCTR4              0xC3
#define ST7735_CMD_PWCTR5              0xC4
#define ST7735_CMD_VMCTR1              0xC5
#define ST7735_CMD_GMCTRP1             0xE0
#define ST7735_CMD_GMCTRN1             0xE1
typedef struct {
    uint8_t command;
    uint8_t data[16];
    uint8_t data_size;
    uint16_t delay_ms;
} st7735_init_cmd_t;

static esp_lcd_panel_io_handle_t s_io = NULL;
static esp_lcd_panel_handle_t s_panel = NULL;
static bool s_initialized = false;

static const st7735_init_cmd_t s_st7735_init_cmds[] = {
    {LCD_CMD_SWRESET, {0}, 0, 150},
    {LCD_CMD_SLPOUT,  {0}, 0, 255},
    {ST7735_CMD_FRMCTR1, {0x01, 0x2C, 0x2D}, 3, 0},
    {ST7735_CMD_FRMCTR2, {0x01, 0x2C, 0x2D}, 3, 0},
    {ST7735_CMD_FRMCTR3, {0x01, 0x2C, 0x2D, 0x01, 0x2C, 0x2D}, 6, 0},
    {ST7735_CMD_INVCTR,  {0x07}, 1, 0},
    {ST7735_CMD_PWCTR1,  {0xA2, 0x02, 0x84}, 3, 0},
    {ST7735_CMD_PWCTR2,  {0xC5}, 1, 0},
    {ST7735_CMD_PWCTR3,  {0x0A, 0x00}, 2, 0},
    {ST7735_CMD_PWCTR4,  {0x8A, 0x2A}, 2, 0},
    {ST7735_CMD_PWCTR5,  {0x8A, 0xEE}, 2, 0},
    {ST7735_CMD_VMCTR1,  {0x0E}, 1, 0},
    {LCD_CMD_COLMOD,     {ST7735_COLMOD_RGB565}, 1, 0},
    {ST7735_CMD_GMCTRP1, {0x0F, 0x1A, 0x0F, 0x18, 0x2F, 0x28, 0x20, 0x22,
                          0x1F, 0x1B, 0x23, 0x37, 0x00, 0x07, 0x02, 0x10}, 16, 0},
    {ST7735_CMD_GMCTRN1, {0x0F, 0x1B, 0x0F, 0x17, 0x33, 0x2C, 0x29, 0x2E,
                          0x30, 0x30, 0x39, 0x3F, 0x00, 0x07, 0x03, 0x10}, 16, 0},
    {LCD_CMD_NORON,      {0}, 0, 10},
    {LCD_CMD_DISPON,     {0}, 0, 100},
};

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

static void cleanup_panel_resources(void)
{
    if (s_panel != NULL) {
        esp_lcd_panel_del(s_panel);
        s_panel = NULL;
    }

    if (s_io != NULL) {
        esp_lcd_panel_io_del(s_io);
        s_io = NULL;
    }
}

static esp_err_t init_spi_bus(void)
{
    const spi_bus_config_t bus_config = {
        .sclk_io_num = BSP_TFT_CLK_GPIO,
        .mosi_io_num = BSP_TFT_MOSI_GPIO,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = BSP_TFT_H_RES * BSP_TFT_DRAW_BUFFER_LINES * sizeof(uint16_t),
    };

    esp_err_t ret = spi_bus_initialize(BSP_TFT_SPI_HOST, &bus_config, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "spi bus init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    return ESP_OK;
}

static esp_err_t init_panel_io(const bsp_tft_callbacks_t *callbacks)
{
    const esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = BSP_TFT_DC_GPIO,
        .cs_gpio_num = BSP_TFT_CS_GPIO,
        .pclk_hz = BSP_TFT_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = BSP_TFT_CMD_BITS,
        .lcd_param_bits = BSP_TFT_PARAM_BITS,
        .spi_mode = 0,
        .trans_queue_depth = BSP_TFT_SPI_TRANS_QUEUE_DEPTH,
        .on_color_trans_done = callbacks ? callbacks->on_color_trans_done : NULL,
        .user_ctx = callbacks ? callbacks->user_ctx : NULL,
    };

    esp_err_t ret = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BSP_TFT_SPI_HOST, &io_config, &s_io);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "panel io init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    return ESP_OK;
}

static esp_err_t init_panel_driver(void)
{
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = BSP_TFT_RST_GPIO,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };

    esp_err_t ret = esp_lcd_new_panel_st7789(s_io, &panel_config, &s_panel);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "panel driver init failed: %s", esp_err_to_name(ret));
        cleanup_panel_resources();
        return ret;
    }

    return ESP_OK;
}

static esp_err_t send_st7735_init_sequence(void)
{
    for (size_t i = 0; i < sizeof(s_st7735_init_cmds) / sizeof(s_st7735_init_cmds[0]); ++i) {
        const st7735_init_cmd_t *cmd = &s_st7735_init_cmds[i];
        const uint8_t *payload = cmd->data_size > 0 ? cmd->data : NULL;

        esp_err_t ret = esp_lcd_panel_io_tx_param(s_io, cmd->command, payload, cmd->data_size);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "st7735 init cmd 0x%02X failed: %s", cmd->command, esp_err_to_name(ret));
            return ret;
        }

        if (cmd->delay_ms > 0U) {
            vTaskDelay(pdMS_TO_TICKS(cmd->delay_ms == 255U ? 500U : cmd->delay_ms));
        }
    }

    return ESP_OK;
}

static esp_err_t apply_st7735_compat_settings(void)
{
    esp_err_t ret = send_st7735_init_sequence();
    if (ret != ESP_OK) {
        return ret;
    }

    ret = esp_lcd_panel_swap_xy(s_panel, ST7735_SWAP_XY);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "st7735 swap_xy failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_lcd_panel_mirror(s_panel, ST7735_MIRROR_X, ST7735_MIRROR_Y);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "st7735 mirror failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_lcd_panel_set_gap(s_panel, ST7735_X_GAP, ST7735_Y_GAP);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "st7735 gap setup failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_lcd_panel_invert_color(s_panel, ST7735_INVERT_COLOR);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "st7735 invert setup failed: %s", esp_err_to_name(ret));
        return ret;
    }

    return ESP_OK;
}

static esp_err_t startup_panel(void)
{
    esp_err_t ret = esp_lcd_panel_reset(s_panel);
    if (ret == ESP_OK) {
        ret = esp_lcd_panel_init(s_panel);
    }
    if (ret == ESP_OK) {
        ret = apply_st7735_compat_settings();
    }
    if (ret == ESP_OK) {
        ret = esp_lcd_panel_disp_on_off(s_panel, true);
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "panel startup failed: %s", esp_err_to_name(ret));
        cleanup_panel_resources();
        return ret;
    }

    return ESP_OK;
}

static void log_panel_ready(void)
{
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
    ESP_LOGI(TAG,
             "ST7735 compat settings: rgb565_colmod=0x%02X swap_xy=%d mirror_x=%d mirror_y=%d gap=(%d,%d) invert=%d",
             ST7735_COLMOD_RGB565,
             ST7735_SWAP_XY,
             ST7735_MIRROR_X,
             ST7735_MIRROR_Y,
             ST7735_X_GAP,
             ST7735_Y_GAP,
             ST7735_INVERT_COLOR);
    ESP_LOGW(TAG, "If the panel is still shifted or upside-down, adjust ST7735 gap/orientation for your module variant");
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

    ret = init_spi_bus();
    if (ret != ESP_OK) {
        return ret;
    }

    ret = init_panel_io(callbacks);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = init_panel_driver();
    if (ret != ESP_OK) {
        return ret;
    }

    ret = startup_panel();
    if (ret != ESP_OK) {
        return ret;
    }

    ret = BSP_TFT_SetBacklight(true);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "backlight enable failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_initialized = true;
    log_panel_ready();
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
    return gpio_set_level(BSP_TFT_BL_GPIO, on ? 1 : 0);
}
