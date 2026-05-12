/**
 * @file INMP441.c
 * @brief INMP441 I2S 数字麦克风 BSP 驱动实现。
 */

#include "BSP_INMP441.h"

#include <stdlib.h>

#include "driver/i2s_std.h"
#include "esp_log.h"

#define BSP_INMP441_LEVEL_SAMPLE_COUNT 256U

static const char *TAG = "BSP_INMP441";
static i2s_chan_handle_t s_rx_chan = NULL;
static bool s_initialized = false;

static int32_t abs_i32(int32_t value)
{
    if (value == INT32_MIN) {
        return INT32_MAX;
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

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = BSP_INMP441_DEFAULT_DMA_DESC_NUM;
    chan_cfg.dma_frame_num = BSP_INMP441_DEFAULT_DMA_FRAME_NUM;

    esp_err_t ret = i2s_new_channel(&chan_cfg, NULL, &s_rx_chan);
    if (ret != ESP_OK) {
        return ret;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(config->sample_rate_hz),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = config->bclk_gpio,
            .ws = config->ws_gpio,
            .dout = I2S_GPIO_UNUSED,
            .din = config->data_in_gpio,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    /* INMP441 的 L/R 接 GND 时输出左声道。 */
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;

    ret = i2s_channel_init_std_mode(s_rx_chan, &std_cfg);
    if (ret != ESP_OK) {
        i2s_del_channel(s_rx_chan);
        s_rx_chan = NULL;
        return ret;
    }

    ret = i2s_channel_enable(s_rx_chan);
    if (ret != ESP_OK) {
        i2s_del_channel(s_rx_chan);
        s_rx_chan = NULL;
        return ret;
    }

    s_initialized = true;
    ESP_LOGI(TAG,
             "init success: bclk_gpio=%d ws_gpio=%d din_gpio=%d sample_rate=%" PRIu32,
             config->bclk_gpio,
             config->ws_gpio,
             config->data_in_gpio,
             config->sample_rate_hz);
    return ESP_OK;
}

esp_err_t BSP_INMP441_Deinit(void)
{
    if (!s_initialized) {
        return ESP_OK;
    }

    esp_err_t ret = i2s_channel_disable(s_rx_chan);
    esp_err_t del_ret = i2s_del_channel(s_rx_chan);
    s_rx_chan = NULL;
    s_initialized = false;

    if (ret != ESP_OK) {
        return ret;
    }
    return del_ret;
}

esp_err_t BSP_INMP441_ReadSamples(int32_t *samples, size_t sample_count, size_t *samples_read, uint32_t timeout_ms)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (samples == NULL || sample_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }

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
    for (size_t i = 0; i < count; ++i) {
        /* INMP441 常见为 24-bit 有效数据放在 32-bit slot 高位，右移后便于观察幅度。 */
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
    level->mean_abs = (int32_t)(abs_sum / (int64_t)samples_read);
    level->sample_count = samples_read;
    return ESP_OK;
}
