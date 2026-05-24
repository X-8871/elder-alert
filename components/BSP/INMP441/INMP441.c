/**
 * @file INMP441.c
 * @brief INMP441 I2S 数字 MEMS 麦克风 BSP 驱动实现。
 *
 * 【学弟必读：I2S 在 ESP-IDF 中的使用】
 * ESP-IDF 5.x 使用 "channel" 的概念来管理 I2S：
 * - `i2s_new_channel()` → 创建 I2S 通道（RX 接收 或 TX 发送）
 * - `i2s_channel_init_std_mode()` → 设置为标准 I2S 模式（Philips 格式）
 * - `i2s_channel_enable()` → 启动通道
 * - `i2s_channel_read()` → 从 RX 通道读取采样数据
 *
 * 【关键配置：slot_mask = I2S_STD_SLOT_LEFT】
 * INMP441 的 L/R 引脚接 GND，所以它只输出左声道数据。
 * slot_mask 设为 LEFT 后，驱动只从 WS=低电平期间读取有效数据，
 * 忽略 WS=高电平期间的无效数据。
 */

#include "BSP_INMP441.h"

#include <stdlib.h>

#include "driver/i2s_std.h"  /* ESP-IDF 标准 I2S 模式 API */
#include "esp_log.h"

#define BSP_INMP441_LEVEL_SAMPLE_COUNT 256U  /* 音量检测用 256 个采样 */

static const char *TAG = "BSP_INMP441";
static i2s_chan_handle_t s_rx_chan = NULL;  /* I2S 接收通道句柄 */
static bool s_initialized = false;

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
