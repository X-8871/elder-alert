/**
 * @file INMP441.c
 * @brief INMP441 I2S 数字 MEMS 麦克风 BSP 驱动实现。
 */

#include "BSP_INMP441.h"

#include <stdlib.h>

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define BSP_INMP441_LEVEL_SAMPLE_COUNT 256U  /* 音量检测用 256 个采样 */

static const char *TAG = "BSP_INMP441";
static i2s_chan_handle_t s_rx_chan = NULL;  /* I2S 接收通道句柄 */
static bool s_initialized = false;
static gpio_num_t s_bclk_gpio = -1;
static gpio_num_t s_ws_gpio = -1;
static gpio_num_t s_din_gpio = -1;

/** 求绝对值——处理 INT32_MIN 的溢出问题 */
static int32_t abs_i32(int32_t value)
{
    if (value == INT32_MIN) {
        return INT32_MAX;  /* -2147483648 取绝对值会溢出，特殊处理 */
    }
    return value < 0 ? -value : value;
}

bool BSP_INMP441_IsInitialized(void)
{
    return s_initialized;
}

esp_err_t BSP_INMP441_Init(const bsp_inmp441_config_t *config)
{
    if (s_initialized) {
        return ESP_OK;
    }
    if (config == NULL ||
        config->bclk_gpio < 0 ||
        config->ws_gpio < 0 ||
        config->data_in_gpio < 0 ||
        config->sample_rate_hz == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 1. 创建 I2S RX 通道（I2S_NUM_AUTO = 自动分配 I2S 控制器） */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = BSP_INMP441_DEFAULT_DMA_DESC_NUM;
    chan_cfg.dma_frame_num = BSP_INMP441_DEFAULT_DMA_FRAME_NUM;

    esp_err_t ret = i2s_new_channel(&chan_cfg, NULL, &s_rx_chan);  /* TX=NULL, RX=s_rx_chan */
    if (ret != ESP_OK) {
        return ret;
    }

    /* 2. 配置标准 I2S 模式：16kHz, 32-bit 数据宽, 单声道, 左声道 slot */
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(config->sample_rate_hz),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                         I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,         /* 不需要主时钟 */
            .bclk = config->bclk_gpio,
            .ws = config->ws_gpio,
            .dout = I2S_GPIO_UNUSED,         /* RX 不需要 DOUT */
            .din = config->data_in_gpio,     /* 麦克风数据从这里输入 */
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;  /* 只取左声道（INMP441 L/R 接 GND） */

    ret = i2s_channel_init_std_mode(s_rx_chan, &std_cfg);
    if (ret != ESP_OK) {
        i2s_del_channel(s_rx_chan);
        s_rx_chan = NULL;
        return ret;
    }

    /* 3. 启动 I2S 通道 */
    ret = i2s_channel_enable(s_rx_chan);
    if (ret != ESP_OK) {
        i2s_del_channel(s_rx_chan);
        s_rx_chan = NULL;
        return ret;
    }

    s_initialized = true;
    s_bclk_gpio = config->bclk_gpio;
    s_ws_gpio = config->ws_gpio;
    s_din_gpio = config->data_in_gpio;
    ESP_LOGI(TAG,
             "init success: bclk_gpio=%d ws_gpio=%d din_gpio=%d sample_rate=%" PRIu32,
             config->bclk_gpio, config->ws_gpio, config->data_in_gpio, config->sample_rate_hz);
    return ESP_OK;
}

esp_err_t BSP_INMP441_Deinit(void)
{
    if (!s_initialized) {
        return ESP_OK;
    }

    /* 先关通道，再删通道。即使 disable 失败也继续删除 */
    esp_err_t ret = i2s_channel_disable(s_rx_chan);
    esp_err_t del_ret = i2s_del_channel(s_rx_chan);
    s_rx_chan = NULL;
    s_initialized = false;

    /* 关键：reset 共享 GPIO（BCLK/WS），把它们从 I2S 矩阵中释放出来。
     * 否则下次 INMP441 或 MAX98357A Init 时，GPIO 仍处于上一个 I2S 控制器的
     * 矩阵模式，导致时钟信号无法正确输出，麦克风读到全零。 */
    if (s_bclk_gpio >= 0) {
        gpio_reset_pin(s_bclk_gpio);
    }
    if (s_ws_gpio >= 0) {
        gpio_reset_pin(s_ws_gpio);
    }
    if (s_din_gpio >= 0) {
        gpio_reset_pin(s_din_gpio);
    }

    /* 给 GPIO 状态一个短暂的稳定时间 */
    vTaskDelay(pdMS_TO_TICKS(50));

    s_bclk_gpio = -1;
    s_ws_gpio = -1;
    s_din_gpio = -1;

    if (ret != ESP_OK) {
        return ret;
    }
    return del_ret;
}

esp_err_t BSP_INMP441_ReadSamples(int32_t *samples, size_t sample_count,
                                  size_t *samples_read, uint32_t timeout_ms)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (samples == NULL || sample_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 从 I2S RX 通道读取采样。每个采样 4 字节（32-bit slot）× sample_count */
    size_t bytes_read = 0;
    esp_err_t ret = i2s_channel_read(s_rx_chan,
                                     samples,
                                     sample_count * sizeof(samples[0]),
                                     &bytes_read,
                                     timeout_ms);
    if (ret != ESP_OK) {
        if (samples_read != NULL) {
            *samples_read = 0;
        }
        return ret;
    }

    size_t count = bytes_read / sizeof(samples[0]);

    /* INMP441 数据在 32-bit slot 的高 24 位 → 右移 8 位对齐到低 24 位 */
    for (size_t i = 0; i < count; ++i) {
        samples[i] >>= 8;
    }

    if (samples_read != NULL) {
        *samples_read = count;
    }
    return ESP_OK;
}

esp_err_t BSP_INMP441_ReadLevel(bsp_inmp441_level_t *level, uint32_t timeout_ms)
{
    if (level == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 读取 256 个采样，计算峰峰值和平均值 */
    int32_t samples[BSP_INMP441_LEVEL_SAMPLE_COUNT] = {0};
    size_t samples_read = 0;
    esp_err_t ret = BSP_INMP441_ReadSamples(samples,
                                            BSP_INMP441_LEVEL_SAMPLE_COUNT,
                                            &samples_read,
                                            timeout_ms);
    if (ret != ESP_OK) {
        return ret;
    }
    if (samples_read == 0) {
        return ESP_ERR_TIMEOUT;
    }

    /* 遍历采样，累计绝对值和找最大值 */
    int64_t abs_sum = 0;
    int32_t peak_abs = 0;
    for (size_t i = 0; i < samples_read; ++i) {
        int32_t abs_value = abs_i32(samples[i]);
        if (abs_value > peak_abs) {
            peak_abs = abs_value;
        }
        abs_sum += abs_value;
    }

    level->peak_abs = peak_abs;
    level->mean_abs = (int32_t)(abs_sum / (int64_t)samples_read);  /* 平均值 = 总和 / 采样数 */
    level->sample_count = samples_read;
    return ESP_OK;
}
