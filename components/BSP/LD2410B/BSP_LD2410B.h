#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BSP_LD2410B_DEFAULT_BAUD_RATE 256000

typedef enum {
    BSP_LD2410B_TARGET_NONE = 0,
    BSP_LD2410B_TARGET_MOVING = 1,
    BSP_LD2410B_TARGET_STATIONARY = 2,
    BSP_LD2410B_TARGET_MOVING_AND_STATIONARY = 3,
} bsp_ld2410b_target_state_t;

typedef struct {
    uart_port_t uart_num;
    gpio_num_t tx_gpio;
    gpio_num_t rx_gpio;
    int baud_rate;
} bsp_ld2410b_config_t;

typedef struct {
    bsp_ld2410b_target_state_t target_state;
    bool presence;
    bool moving_target;
    bool stationary_target;
    uint16_t moving_distance_cm;
    uint8_t moving_energy;
    uint16_t stationary_distance_cm;
    uint8_t stationary_energy;
    uint16_t detection_distance_cm;
} bsp_ld2410b_status_t;

esp_err_t BSP_LD2410B_Init(const bsp_ld2410b_config_t *config);
esp_err_t BSP_LD2410B_Deinit(void);
esp_err_t BSP_LD2410B_ReadStatus(bsp_ld2410b_status_t *status, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
