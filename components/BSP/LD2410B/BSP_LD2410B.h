/**
 * @file BSP_LD2410B.h
 * @brief LD2410B 毫米波人体存在传感器驱动——通过 UART 串口读取目标状态。
 *
 * 【学弟必读：LD2410B 是什么？】
 * LD2410B 是一个 24GHz 毫米波雷达模块，可以穿透衣物等遮挡物检测"人体存在"。
 * 它能区分：
 * - **无人** (none)：探测区域没有目标
 * - **运动目标** (moving)：有人在走动或大幅度活动
 * - **静止目标** (stationary)：人坐着或躺着不动（普通红外 PIR 检测不到的状态！）
 * - **运动+静止** (both)：两种情况同时存在
 *
 * 【通信方式】
 * 模块默认以 256000 bps 的波特率通过 UART 持续发送数据帧。
 * 每帧包含：帧头(4B) + 数据长度(2B) + 数据 + 帧尾(4B)。
 * 数据中包括目标状态、运动/静止距离(cm)和能量值。
 *
 * 【与普通红外(AM312)的区别】
 * AM312 只能检测"有没有人动"，人静止坐着就检测不到。
 * LD2410B 能检测静止人体存在，是本案的核心升级传感器。
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BSP_LD2410B_DEFAULT_BAUD_RATE 256000  /* 模块默认波特率 */

/** 目标状态枚举——和模块数据手册中的定义一致 */
typedef enum {
    BSP_LD2410B_TARGET_NONE = 0,                  /* 无人 */
    BSP_LD2410B_TARGET_MOVING = 1,                 /* 仅有运动目标 */
    BSP_LD2410B_TARGET_STATIONARY = 2,             /* 仅有静止目标 */
    BSP_LD2410B_TARGET_MOVING_AND_STATIONARY = 3,  /* 运动+静止 */
} bsp_ld2410b_target_state_t;

/** 初始化配置 */
typedef struct {
    uart_port_t uart_num;  /* UART 端口号，填 UART_NUM_1（不要和调试串口 UART_NUM_0 冲突） */
    gpio_num_t tx_gpio;    /* ESP32 的 TX 引脚 → 接模块的 RX */
    gpio_num_t rx_gpio;    /* ESP32 的 RX 引脚 → 接模块的 TX */
    int baud_rate;         /* 波特率，0 表示使用默认的 256000 */
} bsp_ld2410b_config_t;

/** 一次完整的目标状态数据 */
typedef struct {
    bsp_ld2410b_target_state_t target_state;  /* 目标综合状态 */
    bool presence;                             /* 是否有人存在 */
    bool moving_target;                        /* 是否有运动目标 */
    bool stationary_target;                    /* 是否有静止目标 */
    uint16_t moving_distance_cm;               /* 运动目标距离 (cm) */
    uint8_t moving_energy;                     /* 运动目标能量值 */
    uint16_t stationary_distance_cm;           /* 静止目标距离 (cm) */
    uint8_t stationary_energy;                 /* 静止目标能量值 */
    uint16_t detection_distance_cm;            /* 探测最大距离 (cm) */
} bsp_ld2410b_status_t;

esp_err_t BSP_LD2410B_Init(const bsp_ld2410b_config_t *config);
esp_err_t BSP_LD2410B_Deinit(void);

/**
 * @brief 读取一帧完整的雷达状态数据。
 * @param status    输出：目标状态
 * @param timeout_ms 等待超时 (ms)，模块大约每 100ms 左右发一帧
 * @return ESP_OK 成功解析一帧；ESP_ERR_TIMEOUT 超时无数据
 */
esp_err_t BSP_LD2410B_ReadStatus(bsp_ld2410b_status_t *status, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
