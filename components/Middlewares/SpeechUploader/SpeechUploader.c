/**
 * @file SpeechUploader.c
 * @brief 录制短 WAV 并通过 HTTP POST 上传到 /api/speech/transcribe。
 */

#include "SpeechUploader.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "WiFiManager.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define SPEECH_UPLOADER_URL "http://spectator0618.online:3000/api/speech/transcribe"
#define SPEECH_UPLOADER_DEVICE_TOKEN ""
#define SPEECH_UPLOADER_TIMEOUT_MS 45000
#define SPEECH_UPLOADER_WAV_HEADER_BYTES 44U
#define SPEECH_UPLOADER_BYTES_PER_SAMPLE 2U
#define SPEECH_UPLOADER_WARMUP_MS         300U   /* I2S 预热：丢弃前 300ms 数据 */
#define SPEECH_UPLOADER_SETTLE_MS         500U   /* 显示录音界面后等 500ms 让用户准备 */

static const char *TAG = "SpeechUploader";
static bool s_initialized = false;
static uint32_t s_sample_rate_hz = BSP_INMP441_DEFAULT_SAMPLE_RATE_HZ;
static speech_uploader_phase_callback_t s_phase_callback = NULL;
static void *s_phase_callback_ctx = NULL;

static void notify_phase(speech_uploader_phase_t phase)
{
    if (s_phase_callback != NULL) {
        s_phase_callback(phase, s_phase_callback_ctx);
    }
}

static int16_t clamp_i16(int32_t value)
{
    if (value > INT16_MAX) {
        return INT16_MAX;
    }
    if (value < INT16_MIN) {
        return INT16_MIN;
    }
    return (int16_t)value;
}

static void write_u16_le(uint8_t *buffer, uint16_t value)
{
    buffer[0] = (uint8_t)(value & 0xFFU);
    buffer[1] = (uint8_t)((value >> 8) & 0xFFU);
}

static void write_u32_le(uint8_t *buffer, uint32_t value)
{
    buffer[0] = (uint8_t)(value & 0xFFU);
    buffer[1] = (uint8_t)((value >> 8) & 0xFFU);
    buffer[2] = (uint8_t)((value >> 16) & 0xFFU);
    buffer[3] = (uint8_t)((value >> 24) & 0xFFU);
}

static void build_wav_header(uint8_t *buffer, uint32_t sample_rate_hz, uint32_t pcm_bytes)
{
    const uint16_t channels = 1U;
    const uint16_t bits_per_sample = 16U;
    const uint32_t byte_rate = sample_rate_hz * channels * (bits_per_sample / 8U);
    const uint16_t block_align = channels * (bits_per_sample / 8U);

    memcpy(buffer + 0, "RIFF", 4);
    write_u32_le(buffer + 4, 36U + pcm_bytes);
    memcpy(buffer + 8, "WAVE", 4);
    memcpy(buffer + 12, "fmt ", 4);
    write_u32_le(buffer + 16, 16U);
    write_u16_le(buffer + 20, 1U);
    write_u16_le(buffer + 22, channels);
    write_u32_le(buffer + 24, sample_rate_hz);
    write_u32_le(buffer + 28, byte_rate);
    write_u16_le(buffer + 32, block_align);
    write_u16_le(buffer + 34, bits_per_sample);
    memcpy(buffer + 36, "data", 4);
    write_u32_le(buffer + 40, pcm_bytes);
}

static esp_err_t post_wav(const uint8_t *wav_data, size_t wav_bytes, int *status_code)
{
    esp_http_client_config_t config = {
        .url = SPEECH_UPLOADER_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = SPEECH_UPLOADER_TIMEOUT_MS,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_FAIL;
    }

    esp_err_t ret = esp_http_client_set_header(client, "Content-Type", "audio/wav");
    if (ret == ESP_OK) {
        ret = esp_http_client_set_header(client, "X-Audio-Format", "wav");
    }
    if (ret == ESP_OK && strlen(SPEECH_UPLOADER_DEVICE_TOKEN) > 0U) {
        ret = esp_http_client_set_header(client, "X-Device-Token", SPEECH_UPLOADER_DEVICE_TOKEN);
    }
    if (ret == ESP_OK) {
        ret = esp_http_client_set_post_field(client, (const char *)wav_data, (int)wav_bytes);
    }
    if (ret == ESP_OK) {
        ret = esp_http_client_perform(client);
    }

    if (status_code != NULL) {
        *status_code = ret == ESP_OK ? esp_http_client_get_status_code(client) : -1;
    }

    esp_http_client_cleanup(client);
    return ret;
}

esp_err_t SpeechUploader_Init(const speech_uploader_config_t *config)
{
    if (s_initialized) {
        return ESP_OK;
    }
    if (config == NULL || config->sample_rate_hz == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    const bsp_inmp441_config_t mic_config = {
        .bclk_gpio = config->bclk_gpio,
        .ws_gpio = config->ws_gpio,
        .data_in_gpio = config->data_in_gpio,
        .sample_rate_hz = config->sample_rate_hz,
    };

    esp_err_t ret = BSP_INMP441_Init(&mic_config);
    if (ret != ESP_OK) {
        return ret;
    }

    s_sample_rate_hz = config->sample_rate_hz;
    s_initialized = true;
    return ESP_OK;
}

esp_err_t SpeechUploader_Deinit(void)
{
    if (!s_initialized) {
        return ESP_OK;
    }

    esp_err_t ret = BSP_INMP441_Deinit();
    if (ret == ESP_OK) {
        s_initialized = false;
        s_sample_rate_hz = BSP_INMP441_DEFAULT_SAMPLE_RATE_HZ;
    }
    return ret;
}

void SpeechUploader_SetPhaseCallback(speech_uploader_phase_callback_t callback, void *user_ctx)
{
    s_phase_callback = callback;
    s_phase_callback_ctx = user_ctx;
}

esp_err_t SpeechUploader_RecordWavAndUpload(uint32_t record_ms)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!WiFiManager_IsConnected()) {
        ESP_LOGW(TAG, "skip speech upload, Wi-Fi not connected");
        return ESP_ERR_INVALID_STATE;
    }

    if (record_ms == 0U) {
        record_ms = SPEECH_UPLOADER_DEFAULT_RECORD_MS;
    }
    if (record_ms > SPEECH_UPLOADER_MAX_RECORD_MS) {
        record_ms = SPEECH_UPLOADER_MAX_RECORD_MS;
    }

    const size_t sample_count = ((size_t)s_sample_rate_hz * record_ms) / 1000U;
    const size_t pcm_bytes = sample_count * SPEECH_UPLOADER_BYTES_PER_SAMPLE;
    const size_t wav_bytes = SPEECH_UPLOADER_WAV_HEADER_BYTES + pcm_bytes;
    uint8_t *wav_data = (uint8_t *)malloc(wav_bytes);
    int32_t *samples = (int32_t *)malloc(BSP_INMP441_DEFAULT_DMA_FRAME_NUM * sizeof(samples[0]));
    if (wav_data == NULL || samples == NULL) {
        free(wav_data);
        free(samples);
        return ESP_ERR_NO_MEM;
    }

    build_wav_header(wav_data, s_sample_rate_hz, (uint32_t)pcm_bytes);
    notify_phase(SPEECH_UPLOADER_PHASE_RECORDING);

    /* 等待 500ms 让用户看到"录音中"界面后准备开口 */
    vTaskDelay(pdMS_TO_TICKS(SPEECH_UPLOADER_SETTLE_MS));

    /* I2S 预热：读取并丢弃前 300ms 数据，避免 DMA 启动伪影 */
    const size_t warmup_samples = ((size_t)s_sample_rate_hz * SPEECH_UPLOADER_WARMUP_MS) / 1000U;
    size_t warmed = 0;
    while (warmed < warmup_samples) {
        size_t batch = BSP_INMP441_DEFAULT_DMA_FRAME_NUM;
        if (batch > warmup_samples - warmed) {
            batch = warmup_samples - warmed;
        }
        size_t read = 0;
        esp_err_t warm_ret = BSP_INMP441_ReadSamples(samples, batch, &read, 1000);
        if (warm_ret != ESP_OK || read == 0U) {
            free(wav_data);
            free(samples);
            ESP_LOGW(TAG, "I2S warmup failed: %s", esp_err_to_name(warm_ret));
            return ESP_FAIL;
        }
        warmed += read;
    }
    ESP_LOGI(TAG, "I2S warmup done, discarded %u samples (%ums)",
             (unsigned)warmed, SPEECH_UPLOADER_WARMUP_MS);

    /* 正式录音 */
    size_t written_samples = 0;
    int32_t peak_abs = 0;
    int64_t abs_sum = 0;
    while (written_samples < sample_count) {
        size_t batch_samples = BSP_INMP441_DEFAULT_DMA_FRAME_NUM;
        size_t remaining = sample_count - written_samples;
        if (batch_samples > remaining) {
            batch_samples = remaining;
        }

        size_t samples_read = 0;
        esp_err_t ret = BSP_INMP441_ReadSamples(samples, batch_samples, &samples_read, 1000);
        if (ret != ESP_OK) {
            free(wav_data);
            free(samples);
            return ret;
        }
        if (samples_read == 0U) {
            free(wav_data);
            free(samples);
            return ESP_ERR_TIMEOUT;
        }

        for (size_t i = 0; i < samples_read; ++i) {
            int32_t raw = samples[i] >> 8;
            int16_t pcm_sample = clamp_i16(raw);
            size_t offset = SPEECH_UPLOADER_WAV_HEADER_BYTES +
                            ((written_samples + i) * SPEECH_UPLOADER_BYTES_PER_SAMPLE);
            write_u16_le(wav_data + offset, (uint16_t)pcm_sample);

            int32_t a = raw < 0 ? -raw : raw;
            if (a > peak_abs) { peak_abs = a; }
            abs_sum += a;
        }
        written_samples += samples_read;
    }

    ESP_LOGI(TAG, "recording done: samples=%u peak=%ld mean=%ld",
             (unsigned)written_samples, (long)peak_abs,
             (long)(abs_sum / (int64_t)written_samples));

    int status_code = -1;
    notify_phase(SPEECH_UPLOADER_PHASE_UPLOADING);
    esp_err_t ret = post_wav(wav_data, wav_bytes, &status_code);
    free(wav_data);
    free(samples);

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "speech upload failed: %s", esp_err_to_name(ret));
        return ret;
    }
    if (status_code < 200 || status_code >= 300) {
        ESP_LOGW(TAG, "speech upload rejected http_status=%d", status_code);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG,
             "speech wav uploaded record_ms=%" PRIu32 " sample_rate=%" PRIu32 " bytes=%u",
             record_ms,
             s_sample_rate_hz,
             (unsigned)wav_bytes);
    return ESP_OK;
}
