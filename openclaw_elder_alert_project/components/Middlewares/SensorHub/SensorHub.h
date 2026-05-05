#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_err.h"

#define SENSOR_HUB_I2C_PORT       I2C_NUM_0
#define SENSOR_HUB_I2C_SDA_GPIO   GPIO_NUM_4
#define SENSOR_HUB_I2C_SCL_GPIO   GPIO_NUM_5
#define SENSOR_HUB_I2C_FREQ_HZ    100000
#define SENSOR_HUB_MQ2_ADC_GPIO   GPIO_NUM_1
#define SENSOR_HUB_AM312_GPIO     GPIO_NUM_6

#define SENSOR_HUB_MQ2_MIN_VALID_RAW 10
#define SENSOR_HUB_MQ2_MAX_VALID_RAW 4085

typedef struct {
    bool aht20;
    bool bmp280;
    bool bh1750;
    bool mq2;
    bool am312;
} sensor_hub_status_t;

typedef struct {
    float aht_temperature;
    float humidity;
    float bmp_temperature;
    float pressure;
    uint16_t lux;
    int mq2_raw;
    int mq2_voltage_mv;
    int am312_raw_level;
    bool motion_detected;
} sensor_hub_data_t;

esp_err_t SensorHub_Init(void);
esp_err_t SensorHub_Read(sensor_hub_data_t *data);
void SensorHub_LogData(const sensor_hub_data_t *data);
void SensorHub_PollAndLog(void);
sensor_hub_status_t SensorHub_GetStatus(void);
