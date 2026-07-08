/**
 * @file TFT.c
 * @brief ST7735 1.8寸 160×128 SPI TFT 彩屏 BSP 驱动实现。
 *
 * 【学弟必读：ST7735 初始化流程】
 * TFT 屏幕的初始化是一个严格的顺序过程，不能跳过任何一步：
 *
 * 1. **init_backlight_gpio()** — 配置背光控制引脚，默认关背光
 * 2. **init_spi_bus()** — 初始化 SPI2 总线，配置 CLK/MOSI 引脚，开启 DMA
 * 3. **init_panel_io()** — 创建 LCD 面板 IO 对象，绑定 DC/CS 引脚
 * 4. **init_panel_driver()** — 创建 ST7789 面板驱动实例（复用 ST7789 驱动）
 * 5. **startup_panel()** — 硬件复位 → 初始化 → 发 ST7735 专属命令 → 开显示
 * 6. **SetBacklight(true)** — 打开背光
 *
 * 【ST7735 初始化序列中的每个命令是干什么的？】
 * - SWRESET (0x01)：软件复位
 * - SLPOUT  (0x11)：退出睡眠模式
 * - FRMCTR1/2/3 (0xB1/0xB2/0xB3)：帧率控制（设置刷新速度）
 * - INVCTR  (0xB4)：显示反转控制
 * - PWCTR1~5 (0xC0~0xC4)：电源控制（设置内部升压电路）
 * - VMCTR1  (0xC5)：VCOM 电压控制
 * - COLMOD  (0x3A)：颜色格式设置（RGB565 = 0x05）
 * - GMCTRP1/0xE0, GMCTRN1/0xE1：Gamma 校正表（正/负极性的灰阶映射）
 * - NORON   (0x13)：进入正常显示模式
 * - DISPON  (0x29)：开显示
 *
 * 【Gamma 校正是什么？】
 * 人眼对亮度的感知不是线性的——暗部变化比亮部变化更敏感。
 * Gamma 校正通过非线性映射让颜色在屏幕上看起来更自然。
 * GMCTRP1/0xE0 和 GMCTRN1/0xE1 分别控制正极性和负极性驱动的灰阶电压。
 *
 * 【delay_ms == 255 的特殊含义】
 * ST7735 数据手册要求退出睡眠模式后至少等 500ms。
 * 代码中用 255 作为"500ms"的标记值，因为 uint8 最大只有 255。
 * 正常 delay 不可能是 255（ST7735 手册最大 delay 是 120ms）。
 */

#include "BSP_TFT.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"       /* SPI 主机驱动 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"           /* vTaskDelay */
#include "esp_lcd_io_spi.h"          /* SPI 接口的 LCD IO */
#include "esp_lcd_panel_commands.h"  /* LCD 通用命令宏 */
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"    /* ST7789 面板驱动（兼容 ST7735） */
#include "esp_log.h"

static const char *TAG = "BSP_TFT";

/* ---- SPI/LCD 传输配置 ---- */
#define BSP_TFT_SPI_TRANS_QUEUE_DEPTH  10  /* SPI 传输队列深度 */

/* ---- ST7735 兼容参数：不同模块版本可能需要调整这些值 ---- */
#define ST7735_COLMOD_RGB565           0x05  /* RGB565 = 16-bit 色 */
#define ST7735_X_GAP                   2      /* X 方向像素偏移 */
#define ST7735_Y_GAP                   3      /* Y 方向像素偏移 */
#define ST7735_SWAP_XY                 true   /* X/Y 交换（物理 128×160→逻辑 160×128） */
#define ST7735_MIRROR_X                true   /* 水平镜像 */
#define ST7735_MIRROR_Y                false  /* 不垂直镜像 */
#define ST7735_INVERT_COLOR            false  /* 不反色 */

/* ---- ST7735 扩展命令码（不在 LCD 通用命令中）---- */
#define ST7735_CMD_FRMCTR1             0xB1  /* 帧率控制 1 */
#define ST7735_CMD_FRMCTR2             0xB2  /* 帧率控制 2 */
#define ST7735_CMD_FRMCTR3             0xB3  /* 帧率控制 3 */
#define ST7735_CMD_INVCTR              0xB4  /* 反色控制 */
#define ST7735_CMD_PWCTR1              0xC0  /* 电源控制 1 */
#define ST7735_CMD_PWCTR2              0xC1  /* 电源控制 2 */
#define ST7735_CMD_PWCTR3              0xC2  /* 电源控制 3 */
#define ST7735_CMD_PWCTR4              0xC3  /* 电源控制 4 */
#define ST7735_CMD_PWCTR5              0xC4  /* 电源控制 5 */
#define ST7735_CMD_VMCTR1              0xC5  /* VCOM 电压控制 */
#define ST7735_CMD_GMCTRP1             0xE0  /* 正极性 Gamma 校正 */
#define ST7735_CMD_GMCTRN1             0xE1  /* 负极性 Gamma 校正 */

/** 初始化序列中每条命令的结构 */
typedef struct {
    uint8_t command;        /* 命令码 */
    uint8_t data[16];       /* 命令参数（最多 16 字节） */
    uint8_t data_size;      /* 实际参数长度 */
    uint16_t delay_ms;      /* 发完命令后等待的时间（255=500ms 特殊标记） */
} st7735_init_cmd_t;

/* ---- 全局状态 ---- */
static esp_lcd_panel_io_handle_t s_io = NULL;      /* LCD 面板 IO 对象 */
static esp_lcd_panel_handle_t s_panel = NULL;       /* LCD 面板驱动实例 */
static bool s_initialized = false;

/* ================================================================
 * ST7735 初始化序列——从数据手册抄来的上电初始化命令表
 * ================================================================
 * 这些命令设置：
 *   - 帧率（约 60Hz）
 *   - 电源电压（升压到驱动 LCD 所需的电压）
 *   - VCOM（液晶偏压，影响对比度）
 *   - Gamma 曲线（灰阶到电压的映射）
 *   - 颜色格式 RGB565
 *
 * delay_ms 为 0 表示不等待，255 表示等 500ms（uint8 最大值标记）。
 */
static const st7735_init_cmd_t s_st7735_init_cmds[] = {
    {LCD_CMD_SWRESET, {0}, 0, 150},  /* 软件复位，等 150ms */
    {LCD_CMD_SLPOUT,  {0}, 0, 255},  /* 退出睡眠，等 500ms（255 标记） */
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
    {LCD_CMD_NORON,      {0}, 0, 10},   /* 正常显示模式 */
    {LCD_CMD_DISPON,     {0}, 0, 100},  /* 开显示 */
};

/* ================================================================
 * 初始化子函数
 * ================================================================ */

esp_err_t BSP_TFT_RegisterCallbacks(const bsp_tft_callbacks_t *callbacks)
{
    if (s_io == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 把用户回调包装为 esp_lcd 框架的回调结构体 */
    const esp_lcd_panel_io_callbacks_t io_callbacks = {
        .on_color_trans_done = callbacks ? callbacks->on_color_trans_done : NULL,
    };

    return esp_lcd_panel_io_register_event_callbacks(s_io, &io_callbacks,
                                                     callbacks ? callbacks->user_ctx : NULL);
}

/** 配置背光 GPIO 为输出并默认关闭 */
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

    return BSP_TFT_SetBacklight(false);  /* 默认关背光，等初始化完成再开 */
}

/** 初始化失败时释放已分配的资源 */
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

/** 初始化 SPI2 总线——只有 SCLK 和 MOSI，没有 MISO（TFT 只写不读） */
static esp_err_t init_spi_bus(void)
{
    const spi_bus_config_t bus_config = {
        .sclk_io_num = BSP_TFT_CLK_GPIO,
        .mosi_io_num = BSP_TFT_MOSI_GPIO,
        .miso_io_num = -1,       /* 不需要 MISO（TFT 不向 ESP32 发数据） */
        .quadwp_io_num = -1,     /* 不使用 Quad SPI */
        .quadhd_io_num = -1,
        .max_transfer_sz = BSP_TFT_H_RES * BSP_TFT_DRAW_BUFFER_LINES * sizeof(uint16_t),
        /* max_transfer_sz = 160 × 20 × 2 = 6400 字节（DMA 最大传输量） */
    };

    esp_err_t ret = spi_bus_initialize(BSP_TFT_SPI_HOST, &bus_config, SPI_DMA_CH_AUTO);
    /* SPI_DMA_CH_AUTO：自动分配 DMA 通道 */
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "spi bus init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    return ESP_OK;
}

/** 创建 SPI LCD 面板 IO 对象——DC 脚区分命令/数据，CS 脚选通芯片 */
static esp_err_t init_panel_io(const bsp_tft_callbacks_t *callbacks)
{
    const esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = BSP_TFT_DC_GPIO,     /* DC=低电平表示命令，DC=高电平表示数据 */
        .cs_gpio_num = BSP_TFT_CS_GPIO,     /* CS=低电平表示选中芯片 */
        .pclk_hz = BSP_TFT_PIXEL_CLOCK_HZ,   /* SPI 时钟 10MHz */
        .lcd_cmd_bits = BSP_TFT_CMD_BITS,
        .lcd_param_bits = BSP_TFT_PARAM_BITS,
        .spi_mode = 0,                       /* SPI 模式 0：CPOL=0, CPHA=0 */
        .trans_queue_depth = BSP_TFT_SPI_TRANS_QUEUE_DEPTH,
        .on_color_trans_done = callbacks ? callbacks->on_color_trans_done : NULL,
        .user_ctx = callbacks ? callbacks->user_ctx : NULL,
    };

    esp_err_t ret = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BSP_TFT_SPI_HOST,
                                              &io_config, &s_io);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "panel io init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    return ESP_OK;
}

/** 创建 ST7789 面板驱动实例——复位引脚由面板层管理 */
static esp_err_t init_panel_driver(void)
{
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = BSP_TFT_RST_GPIO,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,  /* 像素字节序 R→G→B */
        .bits_per_pixel = 16,                         /* 16-bit RGB565 */
    };

    esp_err_t ret = esp_lcd_new_panel_st7789(s_io, &panel_config, &s_panel);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "panel driver init failed: %s", esp_err_to_name(ret));
        cleanup_panel_resources();
        return ret;
    }

    return ESP_OK;
}

/** 逐条发送 ST7735 初始化命令 */
static esp_err_t send_st7735_init_sequence(void)
{
    for (size_t i = 0; i < sizeof(s_st7735_init_cmds) / sizeof(s_st7735_init_cmds[0]); ++i) {
        const st7735_init_cmd_t *cmd = &s_st7735_init_cmds[i];
        /* data_size==0 表示命令没有参数 */
        const uint8_t *payload = cmd->data_size > 0 ? cmd->data : NULL;

        esp_err_t ret = esp_lcd_panel_io_tx_param(s_io, cmd->command, payload, cmd->data_size);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "st7735 init cmd 0x%02X failed: %s", cmd->command, esp_err_to_name(ret));
            return ret;
        }

        /* delay_ms==255 是"等 500ms"的特殊标记（uint8 最大 255 无法表示 500） */
        if (cmd->delay_ms > 0U) {
            vTaskDelay(pdMS_TO_TICKS(cmd->delay_ms == 255U ? 500U : cmd->delay_ms));
        }
    }

    return ESP_OK;
}

/** 应用 ST7735 兼容性修正参数——gap/mirror/swap_xy/invert */
static esp_err_t apply_st7735_compat_settings(void)
{
    /* 先发 ST7735 专属初始化序列 */
    esp_err_t ret = send_st7735_init_sequence();
    if (ret != ESP_OK) {
        return ret;
    }

    /* X/Y 交换：把物理 128×160 旋转为逻辑 160×128（横屏） */
    ret = esp_lcd_panel_swap_xy(s_panel, ST7735_SWAP_XY);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "st7735 swap_xy failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 镜像修正：让画面方向和外壳安装方向一致 */
    ret = esp_lcd_panel_mirror(s_panel, ST7735_MIRROR_X, ST7735_MIRROR_Y);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "st7735 mirror failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* ST7735 有效像素区有偏移，gap 补偿这个偏移 */
    ret = esp_lcd_panel_set_gap(s_panel, ST7735_X_GAP, ST7735_Y_GAP);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "st7735 gap setup failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 颜色取反（通常是 false，除非屏幕显示为负片） */
    ret = esp_lcd_panel_invert_color(s_panel, ST7735_INVERT_COLOR);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "st7735 invert setup failed: %s", esp_err_to_name(ret));
        return ret;
    }

    return ESP_OK;
}

/** 面板启动：复位 → 初始化 → 发 ST7735 命令 → 开显示 */
static esp_err_t startup_panel(void)
{
    esp_err_t ret = esp_lcd_panel_reset(s_panel);   /* 硬件复位 */
    if (ret == ESP_OK) {
        ret = esp_lcd_panel_init(s_panel);           /* 送通用初始化命令 */
    }
    if (ret == ESP_OK) {
        ret = apply_st7735_compat_settings();        /* 送 ST7735 专属命令 */
    }
    if (ret == ESP_OK) {
        ret = esp_lcd_panel_disp_on_off(s_panel, true);  /* 开显示 */
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "panel startup failed: %s", esp_err_to_name(ret));
        cleanup_panel_resources();
        return ret;
    }

    return ESP_OK;
}

/** 打印 TFT 当前配置参数到串口（方便调试） */
static void log_panel_ready(void)
{
    ESP_LOGI(TAG,
             "ST7735 TFT ready: %ux%u clk=%d mosi=%d dc=%d cs=%d rst=%d bl=%d",
             BSP_TFT_H_RES, BSP_TFT_V_RES,
             BSP_TFT_CLK_GPIO, BSP_TFT_MOSI_GPIO, BSP_TFT_DC_GPIO,
             BSP_TFT_CS_GPIO, BSP_TFT_RST_GPIO, BSP_TFT_BL_GPIO);
    ESP_LOGI(TAG,
             "ST7735 compat settings: rgb565_colmod=0x%02X swap_xy=%d mirror_x=%d mirror_y=%d gap=(%d,%d) invert=%d",
             ST7735_COLMOD_RGB565, ST7735_SWAP_XY, ST7735_MIRROR_X, ST7735_MIRROR_Y,
             ST7735_X_GAP, ST7735_Y_GAP, ST7735_INVERT_COLOR);
    ESP_LOGW(TAG, "If the panel is still shifted or upside-down, adjust ST7735 gap/orientation for your module variant");
}

/* ================================================================
 * 公开接口
 * ================================================================ */

esp_err_t BSP_TFT_Init(const bsp_tft_callbacks_t *callbacks)
{
    if (s_initialized) {
        return ESP_OK;
    }

    /* 初始化严格按顺序执行，任一步失败都直接返回 */
    esp_err_t ret;

    ret = init_backlight_gpio();
    if (ret != ESP_OK) { ESP_LOGW(TAG, "backlight gpio init failed: %s", esp_err_to_name(ret)); return ret; }

    ret = init_spi_bus();
    if (ret != ESP_OK) { return ret; }

    ret = init_panel_io(callbacks);
    if (ret != ESP_OK) { return ret; }

    ret = init_panel_driver();
    if (ret != ESP_OK) { return ret; }

    ret = startup_panel();
    if (ret != ESP_OK) { return ret; }

    /* 启动成功 → 开背光 */
    ret = BSP_TFT_SetBacklight(true);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "backlight enable failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_initialized = true;
    log_panel_ready();
    return ESP_OK;
}

bool BSP_TFT_IsReady(void)       { return s_initialized; }
esp_lcd_panel_handle_t BSP_TFT_GetPanelHandle(void) { return s_panel; }
spi_host_device_t BSP_TFT_GetSpiHost(void) { return BSP_TFT_SPI_HOST; }
uint16_t BSP_TFT_GetWidth(void)  { return BSP_TFT_H_RES; }
uint16_t BSP_TFT_GetHeight(void) { return BSP_TFT_V_RES; }
esp_err_t BSP_TFT_SetBacklight(bool on) { return gpio_set_level(BSP_TFT_BL_GPIO, on ? 1 : 0); }
