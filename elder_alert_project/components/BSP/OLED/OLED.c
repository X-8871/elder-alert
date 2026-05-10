/**
 * @file OLED.c
 * @brief SSD1306 128×64 OLED 显示驱动实现，I2C 通信，内置 5×7 ASCII 字模。
 *
 * 使用页寻址帧缓冲区（128×64/8 = 1024 字节），通过 esp_lcd 组件操作 SSD1306 控制器。
 * 字模表覆盖数字、大写字母和少量符号，不支持中文和小写字母。
 */

#include "BSP_OLED.h"

#include <stdint.h>
#include <string.h>

#include "BSP_I2C.h"
#include "i2cdev.h"
#include "esp_lcd_io_i2c.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_ssd1306.h"
#include "esp_log.h"

#define BSP_OLED_I2C_ADDRESS 0x3C

typedef struct {
    char ch;
    uint8_t rows[7];
} oled_glyph_t;

static const char *TAG = "BSP_OLED";

static esp_lcd_panel_io_handle_t s_io = NULL;
static esp_lcd_panel_handle_t s_panel = NULL;
static bool s_initialized = false;
static uint8_t s_framebuffer[BSP_OLED_WIDTH * BSP_OLED_HEIGHT / 8];

static const oled_glyph_t s_glyphs[] = {
    {' ', {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
    {'%', {0x11, 0x02, 0x04, 0x08, 0x11, 0x00, 0x00}},
    {'-', {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00}},
    {'.', {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C}},
    {'/', {0x01, 0x02, 0x04, 0x08, 0x10, 0x00, 0x00}},
    {'0', {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}},
    {'1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'2', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}},
    {'3', {0x1E, 0x01, 0x01, 0x06, 0x01, 0x01, 0x1E}},
    {'4', {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}},
    {'5', {0x1F, 0x10, 0x10, 0x1E, 0x01, 0x01, 0x1E}},
    {'6', {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}},
    {'7', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
    {'8', {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}},
    {'9', {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x1C}},
    {':', {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00}},
    {'A', {0x04, 0x0A, 0x11, 0x11, 0x1F, 0x11, 0x11}},
    {'B', {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}},
    {'C', {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}},
    {'D', {0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C}},
    {'E', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}},
    {'H', {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'I', {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'K', {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}},
    {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}},
    {'M', {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}},
    {'N', {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11}},
    {'O', {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
    {'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}},
    {'Q', {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}},
    {'R', {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}},
    {'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}},
    {'T', {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
    {'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
    {'W', {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A}},
};

static const oled_glyph_t *find_glyph(char ch)
{
    size_t glyph_count = sizeof(s_glyphs) / sizeof(s_glyphs[0]);
    for (size_t i = 0; i < glyph_count; ++i) {
        if (s_glyphs[i].ch == ch) {
            return &s_glyphs[i];
        }
    }

    return &s_glyphs[0];
}

static void set_pixel(uint8_t x, uint8_t y, bool on)
{
    if (x >= BSP_OLED_WIDTH || y >= BSP_OLED_HEIGHT) {
        return;
    }

    size_t index = x + ((size_t)(y / 8U) * BSP_OLED_WIDTH);
    uint8_t bit = 1U << (y % 8U);
    if (on) {
        s_framebuffer[index] |= bit;
    } else {
        s_framebuffer[index] &= (uint8_t)~bit;
    }
}

static void clear_line(uint8_t line_index)
{
    uint8_t y_start = line_index * BSP_OLED_LINE_HEIGHT;
    uint8_t y_end = y_start + BSP_OLED_LINE_HEIGHT;

    for (uint8_t y = y_start; y < y_end && y < BSP_OLED_HEIGHT; ++y) {
        for (uint8_t x = 0; x < BSP_OLED_WIDTH; ++x) {
            set_pixel(x, y, false);
        }
    }
}

static void draw_char(uint8_t x, uint8_t y, char ch)
{
    const oled_glyph_t *glyph = find_glyph(ch);

    for (uint8_t row = 0; row < 7; ++row) {
        for (uint8_t col = 0; col < 5; ++col) {
            bool pixel_on = (glyph->rows[row] & (1U << (4U - col))) != 0U;
            set_pixel((uint8_t)(x + col), (uint8_t)(y + row), pixel_on);
        }
    }
}

static void draw_line_text(uint8_t line_index, const char *text)
{
    clear_line(line_index);

    if (text == NULL) {
        return;
    }

    uint8_t y = (uint8_t)(line_index * BSP_OLED_LINE_HEIGHT + 4U);
    uint8_t x = 1U;

    for (size_t i = 0; text[i] != '\0' && i < BSP_OLED_MAX_LINE_CHARS; ++i) {
        draw_char(x, y, text[i]);
        x = (uint8_t)(x + 6U);
        if (x + 5U >= BSP_OLED_WIDTH) {
            break;
        }
    }
}

esp_err_t BSP_OLED_Init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }
    if (!BSP_I2C_IsInitialized()) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_lcd_panel_io_i2c_config_t io_config = {
        .dev_addr = BSP_OLED_I2C_ADDRESS,
        .control_phase_bytes = 1,
        .dc_bit_offset = 6,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .flags = {
            .dc_low_on_data = 0,
            .disable_control_phase = 0,
        },
        .scl_speed_hz = BSP_I2C_GetClockSpeed(),
    };

    i2c_master_bus_handle_t bus_handle = NULL;
    esp_err_t ret = i2cdev_get_shared_handle(BSP_I2C_GetPort(), (void **)&bus_handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "shared i2c bus unavailable: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_lcd_new_panel_io_i2c(bus_handle, &io_config, &s_io);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "panel io init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    esp_lcd_panel_ssd1306_config_t vendor_config = {
        .height = BSP_OLED_HEIGHT,
    };
    esp_lcd_panel_dev_config_t panel_config = {
        .bits_per_pixel = 1,
        .reset_gpio_num = -1,
        .vendor_config = &vendor_config,
    };

    ret = esp_lcd_new_panel_ssd1306(s_io, &panel_config, &s_panel);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "panel init failed: %s", esp_err_to_name(ret));
        esp_lcd_panel_io_del(s_io);
        s_io = NULL;
        return ret;
    }

    ret = esp_lcd_panel_reset(s_panel);
    if (ret == ESP_OK) {
        ret = esp_lcd_panel_init(s_panel);
    }
    if (ret == ESP_OK) {
        ret = esp_lcd_panel_disp_on_off(s_panel, true);
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "panel start failed: %s", esp_err_to_name(ret));
        esp_lcd_panel_del(s_panel);
        esp_lcd_panel_io_del(s_io);
        s_panel = NULL;
        s_io = NULL;
        return ret;
    }

    memset(s_framebuffer, 0, sizeof(s_framebuffer));
    s_initialized = true;
    ESP_LOGI(TAG, "SSD1306 ready on I2C addr=0x%02X", BSP_OLED_I2C_ADDRESS);

    return BSP_OLED_Clear();
}

bool BSP_OLED_IsReady(void)
{
    return s_initialized;
}

esp_err_t BSP_OLED_Clear(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(s_framebuffer, 0, sizeof(s_framebuffer));
    return esp_lcd_panel_draw_bitmap(s_panel, 0, 0, BSP_OLED_WIDTH, BSP_OLED_HEIGHT, s_framebuffer);
}

esp_err_t BSP_OLED_ShowLines(const char *lines[], size_t line_count)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (lines == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t count = line_count > BSP_OLED_MAX_LINES ? BSP_OLED_MAX_LINES : line_count;
    for (size_t i = 0; i < BSP_OLED_MAX_LINES; ++i) {
        const char *line_text = i < count ? lines[i] : "";
        draw_line_text((uint8_t)i, line_text);
    }

    return esp_lcd_panel_draw_bitmap(s_panel, 0, 0, BSP_OLED_WIDTH, BSP_OLED_HEIGHT, s_framebuffer);
}
