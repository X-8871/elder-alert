/**
 * @file MAX98357A.c
 * @brief MAX98357A I2S 数字功放 BSP 驱动实现。
 */

#include "BSP_MAX98357A.h"

#include <inttypes.h>
#include <stdlib.h>

#include "driver/i2s_std.h"
#include "esp_log.h"

static const char *TAG = "BSP_MAX98357A";
static i2s_chan_handle_t s_tx_chan = NULL;
static bool s_initialized = false;
static uint32_t s_sample_rate_hz = BSP_MAX98357A_DEFAULT_SAMPLE_RATE_HZ;

bool BSP_MAX98357A_IsInitialized(void)
{
    return s_initialized;
}

esp_err_t BSP_MAX98357A_Init(const bsp_max98357a_config_t *config)
{
    if (s_initialized) {
        return ESP_OK;
    }
    if (config == NULL ||
        config->bclk_gpio < 0 ||
        config->ws_gpio < 0 ||
        config->data_out_gpio < 0 ||
        config->sample_rate_hz == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = BSP_MAX98357A_DEFAULT_DMA_DESC_NUM;
    chan_cfg.dma_frame_num = BSP_MAX98357A_DEFAULT_DMA_FRAME_NUM;

    esp_err_t ret = i2s_new_channel(&chan_cfg, &s_tx_chan, NULL);
    if (ret != ESP_OK) {
        return ret;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(config->sample_rate_hz),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = config->bclk_gpio,
            .ws = config->ws_gpio,
            .dout = config->data_out_gpio,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    ret = i2s_channel_init_std_mode(s_tx_chan, &std_cfg);
    if (ret != ESP_OK) {
        i2s_del_channel(s_tx_chan);
        s_tx_chan = NULL;
        return ret;
    }

    ret = i2s_channel_enable(s_tx_chan);
    if (ret != ESP_OK) {
        i2s_del_channel(s_tx_chan);
        s_tx_chan = NULL;
        return ret;
    }

    s_sample_rate_hz = config->sample_rate_hz;
    s_initialized = true;
    ESP_LOGI(TAG,
             "init success: bclk_gpio=%d ws_gpio=%d dout_gpio=%d sample_rate=%" PRIu32,
             config->bclk_gpio,
             config->ws_gpio,
             config->data_out_gpio,
             config->sample_rate_hz);
    return ESP_OK;
}

esp_err_t BSP_MAX98357A_Deinit(void)
{
    if (!s_initialized) {
        return ESP_OK;
    }

    esp_err_t ret = i2s_channel_disable(s_tx_chan);
    esp_err_t del_ret = i2s_del_channel(s_tx_chan);
    s_tx_chan = NULL;
    s_initialized = false;
    s_sample_rate_hz = BSP_MAX98357A_DEFAULT_SAMPLE_RATE_HZ;

    if (ret != ESP_OK) {
        return ret;
    }
    return del_ret;
}

esp_err_t BSP_MAX98357A_WriteMonoSamples(const int16_t *samples, size_t sample_count, uint32_t timeout_ms)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (samples == NULL || sample_count == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    int16_t stereo[BSP_MAX98357A_DEFAULT_DMA_FRAME_NUM * 2U] = {0};
    size_t written_samples = 0;
    while (written_samples < sample_count) {
        size_t batch_samples = sample_count - written_samples;
        if (batch_samples > BSP_MAX98357A_DEFAULT_DMA_FRAME_NUM) {
            batch_samples = BSP_MAX98357A_DEFAULT_DMA_FRAME_NUM;
        }

        for (size_t i = 0; i < batch_samples; ++i) {
            int16_t sample = samples[written_samples + i];
            stereo[(i * 2U) + 0U] = sample;
            stereo[(i * 2U) + 1U] = sample;
        }

        size_t bytes_written = 0;
        esp_err_t ret = i2s_channel_write(s_tx_chan,
                                          stereo,
                                          batch_samples * 2U * sizeof(stereo[0]),
                                          &bytes_written,
                                          timeout_ms);
        if (ret != ESP_OK) {
            return ret;
        }
        if (bytes_written == 0U) {
            return ESP_ERR_TIMEOUT;
        }

        written_samples += bytes_written / (2U * sizeof(stereo[0]));
    }
    return ESP_OK;
}

esp_err_t BSP_MAX98357A_PlayTone(uint32_t frequency_hz, uint32_t duration_ms, int16_t amplitude)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (frequency_hz == 0U || duration_ms == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    int16_t samples[BSP_MAX98357A_DEFAULT_DMA_FRAME_NUM] = {0};
    const size_t total_samples = ((size_t)s_sample_rate_hz * duration_ms) / 1000U;
    uint32_t half_period_samples = s_sample_rate_hz / (frequency_hz * 2U);
    if (half_period_samples == 0U) {
        half_period_samples = 1U;
    }

    size_t generated = 0;
    while (generated < total_samples) {
        size_t batch_samples = total_samples - generated;
        if (batch_samples > BSP_MAX98357A_DEFAULT_DMA_FRAME_NUM) {
            batch_samples = BSP_MAX98357A_DEFAULT_DMA_FRAME_NUM;
        }

        for (size_t i = 0; i < batch_samples; ++i) {
            size_t sample_index = generated + i;
            bool high = ((sample_index / half_period_samples) % 2U) == 0U;
            samples[i] = high ? amplitude : (int16_t)-amplitude;
        }

        esp_err_t ret = BSP_MAX98357A_WriteMonoSamples(samples, batch_samples, 1000);
        if (ret != ESP_OK) {
            return ret;
        }
        generated += batch_samples;
    }
    return ESP_OK;
}

esp_err_t BSP_MAX98357A_PlaySilence(uint32_t duration_ms)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (duration_ms == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    int16_t samples[BSP_MAX98357A_DEFAULT_DMA_FRAME_NUM] = {0};
    size_t remaining = ((size_t)s_sample_rate_hz * duration_ms) / 1000U;
    while (remaining > 0U) {
        size_t batch_samples = remaining;
        if (batch_samples > BSP_MAX98357A_DEFAULT_DMA_FRAME_NUM) {
            batch_samples = BSP_MAX98357A_DEFAULT_DMA_FRAME_NUM;
        }

        esp_err_t ret = BSP_MAX98357A_WriteMonoSamples(samples, batch_samples, 1000);
        if (ret != ESP_OK) {
            return ret;
        }
        remaining -= batch_samples;
    }
    return ESP_OK;
}
