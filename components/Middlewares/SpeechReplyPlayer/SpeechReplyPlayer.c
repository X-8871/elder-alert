/**
 * @file SpeechReplyPlayer.c
 * @brief 下载最新云端 TTS WAV 回复，并通过 MAX98357A 播放。
 */

#include "SpeechReplyPlayer.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "WiFiManager.h"
#include "esp_http_client.h"
#include "esp_log.h"

#define SPEECH_REPLY_PLAYER_URL "http://spectator0618.online:3000/api/speech/reply-audio"
#define SPEECH_REPLY_PLAYER_DEVICE_TOKEN ""
#define SPEECH_REPLY_PLAYER_TIMEOUT_MS 25000
#define SPEECH_REPLY_PLAYER_MAX_WAV_BYTES (512U * 1024U)
#define SPEECH_REPLY_PLAYER_WAV_HEADER_MIN_BYTES 44U
#define SPEECH_REPLY_PLAYER_DOWNLOAD_RETRIES 3U
#define SPEECH_REPLY_PLAYER_RETRY_DELAY_MS 1000U
#define SPEECH_REPLY_PLAYER_WRITE_TIMEOUT_MS 2000
#define SPEECH_REPLY_PLAYER_LEAD_SILENCE_MS 80U
#define SPEECH_REPLY_PLAYER_TAIL_SILENCE_MS 80U

static const char *TAG = "SpeechReplyPlayer";

typedef struct {
    uint32_t sample_rate_hz;
    uint16_t channels;
    uint16_t bits_per_sample;
    uint32_t data_offset;
    uint32_t data_bytes;
} wav_info_t;

static uint16_t read_u16_le(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static uint32_t read_u32_le(const uint8_t *data)
{
    return (uint32_t)data[0] |
           ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) |
           ((uint32_t)data[3] << 24);
}

static esp_err_t parse_wav_info(const uint8_t *wav_data, size_t wav_bytes, wav_info_t *info)
{
    if (wav_data == NULL || info == NULL || wav_bytes < SPEECH_REPLY_PLAYER_WAV_HEADER_MIN_BYTES) {
        return ESP_ERR_INVALID_ARG;
    }
    if (memcmp(wav_data + 0, "RIFF", 4) != 0 || memcmp(wav_data + 8, "WAVE", 4) != 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    bool fmt_found = false;
    bool data_found = false;
    size_t pos = 12U;
    memset(info, 0, sizeof(*info));

    while (pos + 8U <= wav_bytes) {
        const uint8_t *chunk = wav_data + pos;
        uint32_t chunk_size = read_u32_le(chunk + 4);
        size_t chunk_data_pos = pos + 8U;
        if (chunk_data_pos + chunk_size > wav_bytes) {
            return ESP_ERR_INVALID_SIZE;
        }

        if (memcmp(chunk, "fmt ", 4) == 0) {
            if (chunk_size < 16U) {
                return ESP_ERR_INVALID_SIZE;
            }
            uint16_t audio_format = read_u16_le(wav_data + chunk_data_pos + 0);
            info->channels = read_u16_le(wav_data + chunk_data_pos + 2);
            info->sample_rate_hz = read_u32_le(wav_data + chunk_data_pos + 4);
            info->bits_per_sample = read_u16_le(wav_data + chunk_data_pos + 14);
            if (audio_format != 1U || info->channels != 1U || info->bits_per_sample != 16U ||
                info->sample_rate_hz == 0U) {
                return ESP_ERR_NOT_SUPPORTED;
            }
            fmt_found = true;
        } else if (memcmp(chunk, "data", 4) == 0) {
            info->data_offset = (uint32_t)chunk_data_pos;
            info->data_bytes = chunk_size;
            data_found = true;
        }

        pos = chunk_data_pos + chunk_size;
        if ((chunk_size % 2U) != 0U) {
            pos++;
        }
    }

    if (!fmt_found || !data_found || info->data_bytes == 0U || (info->data_bytes % 2U) != 0U) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

static esp_err_t download_reply_wav(uint8_t **wav_data, size_t *wav_bytes, int *status_code)
{
    if (wav_data == NULL || wav_bytes == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *wav_data = NULL;
    *wav_bytes = 0;

    esp_http_client_config_t config = {
        .url = SPEECH_REPLY_PLAYER_URL,
        .method = HTTP_METHOD_GET,
        .timeout_ms = SPEECH_REPLY_PLAYER_TIMEOUT_MS,
        .buffer_size = 2048,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_FAIL;
    }

    esp_err_t ret = ESP_OK;
    if (strlen(SPEECH_REPLY_PLAYER_DEVICE_TOKEN) > 0U) {
        ret = esp_http_client_set_header(client, "X-Device-Token", SPEECH_REPLY_PLAYER_DEVICE_TOKEN);
    }
    if (ret == ESP_OK) {
        ret = esp_http_client_open(client, 0);
    }
    int64_t content_length = -1;
    if (ret == ESP_OK) {
        content_length = esp_http_client_fetch_headers(client);
    }

    int http_status = ret == ESP_OK ? esp_http_client_get_status_code(client) : -1;
    if (status_code != NULL) {
        *status_code = http_status;
    }
    if (ret != ESP_OK) {
        esp_http_client_cleanup(client);
        return ret;
    }
    if (http_status < 200 || http_status >= 300) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }
    if (content_length <= 0 || content_length > SPEECH_REPLY_PLAYER_MAX_WAV_BYTES) {
        ESP_LOGW(TAG, "unexpected reply audio length=%" PRId64, content_length);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t *buffer = (uint8_t *)malloc((size_t)content_length);
    if (buffer == NULL) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    size_t total_read = 0;
    while (total_read < (size_t)content_length) {
        int read_len = esp_http_client_read(client,
                                            (char *)buffer + total_read,
                                            (int)((size_t)content_length - total_read));
        if (read_len < 0) {
            free(buffer);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return ESP_FAIL;
        }
        if (read_len == 0) {
            break;
        }
        total_read += (size_t)read_len;
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (total_read != (size_t)content_length) {
        free(buffer);
        ESP_LOGW(TAG, "reply audio read incomplete expected=%" PRId64 " actual=%u",
                 content_length,
                 (unsigned)total_read);
        return ESP_ERR_INVALID_SIZE;
    }

    *wav_data = buffer;
    *wav_bytes = total_read;
    return ESP_OK;
}

esp_err_t SpeechReplyPlayer_PlayLatest(const bsp_max98357a_config_t *base_amp_config)
{
    if (base_amp_config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!WiFiManager_IsConnected()) {
        ESP_LOGW(TAG, "skip reply audio playback, Wi-Fi not connected");
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t *wav_data = NULL;
    size_t wav_bytes = 0;
    int status_code = -1;
    esp_err_t ret = ESP_FAIL;
    for (uint32_t attempt = 1U; attempt <= SPEECH_REPLY_PLAYER_DOWNLOAD_RETRIES; ++attempt) {
        status_code = -1;
        ret = download_reply_wav(&wav_data, &wav_bytes, &status_code);
        if (ret == ESP_OK) {
            break;
        }

        ESP_LOGW(TAG,
                 "download reply audio attempt=%" PRIu32 " failed: %s http_status=%d",
                 attempt,
                 esp_err_to_name(ret),
                 status_code);
        if (status_code == 404 && attempt < SPEECH_REPLY_PLAYER_DOWNLOAD_RETRIES) {
            vTaskDelay(pdMS_TO_TICKS(SPEECH_REPLY_PLAYER_RETRY_DELAY_MS));
            continue;
        }
        break;
    }

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "download reply audio failed: %s http_status=%d", esp_err_to_name(ret), status_code);
        return ret;
    }

    wav_info_t info = {0};
    ret = parse_wav_info(wav_data, wav_bytes, &info);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "parse reply wav failed: %s bytes=%u", esp_err_to_name(ret), (unsigned)wav_bytes);
        free(wav_data);
        return ret;
    }

    bsp_max98357a_config_t amp_config = *base_amp_config;
    amp_config.sample_rate_hz = info.sample_rate_hz;
    ret = BSP_MAX98357A_Init(&amp_config);
    if (ret != ESP_OK) {
        free(wav_data);
        return ret;
    }

    ret = BSP_MAX98357A_PlaySilence(SPEECH_REPLY_PLAYER_LEAD_SILENCE_MS);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "reply pre-silence failed: %s", esp_err_to_name(ret));
    }

    ESP_LOGI(TAG,
             "play reply wav sample_rate=%" PRIu32 " channels=%u bits=%u bytes=%" PRIu32,
             info.sample_rate_hz,
             info.channels,
             info.bits_per_sample,
             info.data_bytes);
    size_t sample_count = info.data_bytes / sizeof(int16_t);
    ret = BSP_MAX98357A_WriteMonoSamples((const int16_t *)(wav_data + info.data_offset),
                                         sample_count,
                                         SPEECH_REPLY_PLAYER_WRITE_TIMEOUT_MS);
    if (ret == ESP_OK) {
        esp_err_t silence_ret = BSP_MAX98357A_PlaySilence(SPEECH_REPLY_PLAYER_TAIL_SILENCE_MS);
        if (silence_ret != ESP_OK) {
            ESP_LOGW(TAG, "reply tail-silence failed: %s", esp_err_to_name(silence_ret));
        }
    }

    esp_err_t deinit_ret = BSP_MAX98357A_Deinit();
    free(wav_data);
    if (ret != ESP_OK) {
        return ret;
    }
    return deinit_ret;
}
