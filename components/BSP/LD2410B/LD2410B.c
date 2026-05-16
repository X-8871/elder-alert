/**
 * @file LD2410B.c
 * @brief LD2410B 毫米波人体存在模块 UART 最小读取驱动。
 */

#include "BSP_LD2410B.h"

#include <string.h>

#include "esp_log.h"

static const char *TAG = "BSP_LD2410B";

#define LD2410B_FRAME_HEADER_LEN 4
#define LD2410B_FRAME_FOOTER_LEN 4
#define LD2410B_MAX_PAYLOAD_LEN 64

static const uint8_t s_report_header[LD2410B_FRAME_HEADER_LEN] = {0xF4, 0xF3, 0xF2, 0xF1};
static const uint8_t s_report_footer[LD2410B_FRAME_FOOTER_LEN] = {0xF8, 0xF7, 0xF6, 0xF5};

static uart_port_t s_uart_num = UART_NUM_MAX;
static bool s_initialized = false;

static uint16_t read_le16(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

esp_err_t BSP_LD2410B_Init(const bsp_ld2410b_config_t *config)
{
    if (config == NULL || config->uart_num >= UART_NUM_MAX ||
        config->tx_gpio < 0 || config->rx_gpio < 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_initialized) {
        return ESP_OK;
    }

    const uart_config_t uart_config = {
        .baud_rate = config->baud_rate > 0 ? config->baud_rate : BSP_LD2410B_DEFAULT_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t ret = uart_driver_install(config->uart_num, 2048, 0, 0, NULL, 0);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = uart_param_config(config->uart_num, &uart_config);
    if (ret != ESP_OK) {
        uart_driver_delete(config->uart_num);
        return ret;
    }

    ret = uart_set_pin(config->uart_num,
                       config->tx_gpio,
                       config->rx_gpio,
                       UART_PIN_NO_CHANGE,
                       UART_PIN_NO_CHANGE);
    if (ret != ESP_OK) {
        uart_driver_delete(config->uart_num);
        return ret;
    }

    uart_flush_input(config->uart_num);
    s_uart_num = config->uart_num;
    s_initialized = true;

    ESP_LOGI(TAG,
             "init success uart=%d tx_gpio=%d rx_gpio=%d baud=%d",
             config->uart_num,
             config->tx_gpio,
             config->rx_gpio,
             uart_config.baud_rate);
    return ESP_OK;
}

esp_err_t BSP_LD2410B_Deinit(void)
{
    if (!s_initialized) {
        return ESP_OK;
    }

    esp_err_t ret = uart_driver_delete(s_uart_num);
    s_uart_num = UART_NUM_MAX;
    s_initialized = false;
    return ret;
}

static esp_err_t read_exact(uint8_t *buffer, size_t len, uint32_t timeout_ms)
{
    size_t offset = 0;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);

    while (offset < len) {
        TickType_t now = xTaskGetTickCount();
        if ((int32_t)(deadline - now) <= 0) {
            return ESP_ERR_TIMEOUT;
        }

        int read_len = uart_read_bytes(s_uart_num,
                                       buffer + offset,
                                       len - offset,
                                       deadline - now);
        if (read_len < 0) {
            return ESP_FAIL;
        }
        offset += (size_t)read_len;
    }

    return ESP_OK;
}

static esp_err_t wait_report_header(uint32_t timeout_ms)
{
    uint8_t matched = 0;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);

    while (matched < LD2410B_FRAME_HEADER_LEN) {
        TickType_t now = xTaskGetTickCount();
        if ((int32_t)(deadline - now) <= 0) {
            return ESP_ERR_TIMEOUT;
        }

        uint8_t byte = 0;
        int read_len = uart_read_bytes(s_uart_num, &byte, 1, deadline - now);
        if (read_len < 0) {
            return ESP_FAIL;
        }
        if (read_len == 0) {
            continue;
        }

        if (byte == s_report_header[matched]) {
            ++matched;
        } else {
            matched = (byte == s_report_header[0]) ? 1 : 0;
        }
    }

    return ESP_OK;
}

static esp_err_t parse_status_payload(const uint8_t *payload,
                                      uint16_t payload_len,
                                      bsp_ld2410b_status_t *status)
{
    if (payload_len < 13 || payload[0] != 0x02 || payload[1] != 0xAA) {
        return ESP_FAIL;
    }

    uint8_t state = payload[2];
    if (state > BSP_LD2410B_TARGET_MOVING_AND_STATIONARY) {
        state = BSP_LD2410B_TARGET_NONE;
    }

    memset(status, 0, sizeof(*status));
    status->target_state = (bsp_ld2410b_target_state_t)state;
    status->presence = state != BSP_LD2410B_TARGET_NONE;
    status->moving_target = state == BSP_LD2410B_TARGET_MOVING ||
                            state == BSP_LD2410B_TARGET_MOVING_AND_STATIONARY;
    status->stationary_target = state == BSP_LD2410B_TARGET_STATIONARY ||
                                state == BSP_LD2410B_TARGET_MOVING_AND_STATIONARY;
    status->moving_distance_cm = read_le16(&payload[3]);
    status->moving_energy = payload[5];
    status->stationary_distance_cm = read_le16(&payload[6]);
    status->stationary_energy = payload[8];
    status->detection_distance_cm = read_le16(&payload[9]);
    return ESP_OK;
}

esp_err_t BSP_LD2410B_ReadStatus(bsp_ld2410b_status_t *status, uint32_t timeout_ms)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = wait_report_header(timeout_ms);
    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t len_bytes[2] = {0};
    ret = read_exact(len_bytes, sizeof(len_bytes), timeout_ms);
    if (ret != ESP_OK) {
        return ret;
    }

    uint16_t payload_len = read_le16(len_bytes);
    if (payload_len == 0 || payload_len > LD2410B_MAX_PAYLOAD_LEN) {
        return ESP_FAIL;
    }

    uint8_t payload[LD2410B_MAX_PAYLOAD_LEN] = {0};
    ret = read_exact(payload, payload_len, timeout_ms);
    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t footer[LD2410B_FRAME_FOOTER_LEN] = {0};
    ret = read_exact(footer, sizeof(footer), timeout_ms);
    if (ret != ESP_OK) {
        return ret;
    }
    if (memcmp(footer, s_report_footer, sizeof(footer)) != 0) {
        return ESP_FAIL;
    }

    return parse_status_payload(payload, payload_len, status);
}
