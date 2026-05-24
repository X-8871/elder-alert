/**
 * @file LD2410B.c
 * @brief LD2410B 毫米波人体存在模块 UART 最小读取驱动。
 *
 * 【学弟必读：LD2410B 通信协议】
 * 模块通过 UART 以约 100ms 间隔持续输出数据帧，帧格式如下：
 * ┌──────────┬──────────┬──────────┬──────────┐
 * │ 帧头 4B  │ 长度 2B  │ 数据 N B │ 帧尾 4B  │
 * │F4 F3 F2 F1│ 小端序   │ 详见状态  │F8 F7 F6 F5│
 * └──────────┴──────────┴──────────┴──────────┘
 *
 * 【帧同步机制】
 * 因为数据是持续流式发来的（没有请求-响应的概念），接收端需要：
 * 1. 逐字节搜索帧头 F4 F3 F2 F1
 * 2. 找到帧头后再读取后续的长度和数据
 * 3. 验证帧尾 F8 F7 F6 F5
 * 4. 如果帧头帧尾不匹配，丢弃整帧重新同步
 *
 * 【小端序 (Little-Endian)】
 * 数据帧中的多字节数值（如距离、长度）都是"小端序"存储：
 * 低地址存放低字节，高地址存放高字节。例如 0x1234 → [0x34, 0x12]。
 * read_le16() 函数就是用来把 UART 收到的两个字节还原为 16-bit 值的。
 */

#include "BSP_LD2410B.h"

#include <string.h>

#include "esp_log.h"

static const char *TAG = "BSP_LD2410B";

#define LD2410B_FRAME_HEADER_LEN 4   /* 帧头固定 4 字节 */
#define LD2410B_FRAME_FOOTER_LEN 4   /* 帧尾固定 4 字节 */
#define LD2410B_MAX_PAYLOAD_LEN 64   /* 数据部分最大 64 字节 */

/* 帧头/帧尾的魔数——数据手册规定 */
static const uint8_t s_report_header[LD2410B_FRAME_HEADER_LEN] = {0xF4, 0xF3, 0xF2, 0xF1};
static const uint8_t s_report_footer[LD2410B_FRAME_FOOTER_LEN] = {0xF8, 0xF7, 0xF6, 0xF5};

static uart_port_t s_uart_num = UART_NUM_MAX;  /* 当前使用的 UART 端口 */
static bool s_initialized = false;

/**
 * @brief 从两个字节按小端序拼出 16-bit 无符号整数。
 * 例：[0x34, 0x12] → 0x1234
 */
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

    /* 1. 配置 UART 参数：波特率 256000、8N1（8位数据/无校验/1停止位/无流控） */
    const uart_config_t uart_config = {
        .baud_rate = config->baud_rate > 0 ? config->baud_rate : BSP_LD2410B_DEFAULT_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,  /* 模块不需要硬件流控 */
        .source_clk = UART_SCLK_DEFAULT,
    };

    /* 2. 安装 UART 驱动（分配 2048 字节接收缓冲区） */
    esp_err_t ret = uart_driver_install(config->uart_num, 2048, 0, 0, NULL, 0);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = uart_param_config(config->uart_num, &uart_config);
    if (ret != ESP_OK) {
        uart_driver_delete(config->uart_num);
        return ret;
    }

    /* 3. 设置 UART 引脚 */
    ret = uart_set_pin(config->uart_num,
                       config->tx_gpio,
                       config->rx_gpio,
                       UART_PIN_NO_CHANGE,  /* RTS 不配置 */
                       UART_PIN_NO_CHANGE); /* CTS 不配置 */
    if (ret != ESP_OK) {
        uart_driver_delete(config->uart_num);
        return ret;
    }

    /* 4. 清空接收缓冲区（可能有启动时的垃圾数据） */
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

/**
 * @brief 从 UART 缓冲区精确读取 len 个字节（带超时）。
 * 与 uart_read_bytes 的区别：这个函数在读到足够数据前会一直等待，
 * 而非读取"当前可用"的数据就立刻返回。
 */
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

/**
 * @brief 在 UART 流中搜索帧头魔数 F4 F3 F2 F1。
 *
 * 逐字节接收，匹配帧头。匹配失败则回退重新从第一个字节开始匹配。
 * 这是帧同步的关键——不管何时开始读 UART 流，都能锁定下一帧的开头。
 */
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
            continue;  /* 还没数据，继续等 */
        }

        /* 逐个匹配帧头字节：F4 → F3 → F2 → F1 */
        if (byte == s_report_header[matched]) {
            ++matched;
        } else {
            /* 匹配中断，回退检查是否恰好是帧头第一字节 */
            matched = (byte == s_report_header[0]) ? 1 : 0;
        }
    }

    return ESP_OK;
}

/**
 * @brief 解析状态数据体。
 *
 * 数据体格式（13 字节以上）：
 *   [0] = 0x02 (消息类型：工程数据)
 *   [1] = 0xAA (工程消息子类型：目标状态)
 *   [2] = 目标状态 (0=无人 1=运动 2=静止 3=运动+静止)
 *   [3-4] = 运动目标距离 (cm) 小端序
 *   [5] = 运动目标能量
 *   [6-7] = 静止目标距离 (cm) 小端序
 *   [8] = 静止目标能量
 *   [9-10] = 探测距离 (cm) 小端序
 */
static esp_err_t parse_status_payload(const uint8_t *payload,
                                      uint16_t payload_len,
                                      bsp_ld2410b_status_t *status)
{
    if (payload_len < 13 || payload[0] != 0x02 || payload[1] != 0xAA) {
        return ESP_FAIL;  /* 不是目标状态数据包，或数据长度不够 */
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

    /* 完整读取一帧的流程：搜索帧头 → 读长度 → 读数据 → 验证帧尾 → 解析 */

    /* 1. 等帧头 */
    esp_err_t ret = wait_report_header(timeout_ms);
    if (ret != ESP_OK) {
        return ret;
    }

    /* 2. 读数据长度（2 字节，小端序） */
    uint8_t len_bytes[2] = {0};
    ret = read_exact(len_bytes, sizeof(len_bytes), timeout_ms);
    if (ret != ESP_OK) {
        return ret;
    }

    uint16_t payload_len = read_le16(len_bytes);
    if (payload_len == 0 || payload_len > LD2410B_MAX_PAYLOAD_LEN) {
        return ESP_FAIL;  /* 长度异常 */
    }

    /* 3. 读数据体 */
    uint8_t payload[LD2410B_MAX_PAYLOAD_LEN] = {0};
    ret = read_exact(payload, payload_len, timeout_ms);
    if (ret != ESP_OK) {
        return ret;
    }

    /* 4. 读帧尾并校验 */
    uint8_t footer[LD2410B_FRAME_FOOTER_LEN] = {0};
    ret = read_exact(footer, sizeof(footer), timeout_ms);
    if (ret != ESP_OK) {
        return ret;
    }
    if (memcmp(footer, s_report_footer, sizeof(footer)) != 0) {
        return ESP_FAIL;  /* 帧尾不匹配 */
    }

    /* 5. 解析数据 */
    return parse_status_payload(payload, payload_len, status);
}
