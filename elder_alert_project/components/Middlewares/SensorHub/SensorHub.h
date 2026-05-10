/**
 * @file SensorHub.h
 * @brief 传感器中枢，统一管理 AHT20 / BMP280 / BH1750 / MQ2 / AM312 的初始化和数据采集。
 *
 * SensorHub 屏蔽底层各传感器驱动差异，提供统一的数据读取接口和健康状态查询。
 * 单个传感器初始化失败不会阻塞其他传感器，上层可通过 sensor_hub_status_t 判断可用性。
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_err.h"

/* I2C 总线配置，AHT20/BMP280/BH1750/OLED 共享此总线。 */
#define SENSOR_HUB_I2C_PORT       I2C_NUM_0
#define SENSOR_HUB_I2C_SDA_GPIO   GPIO_NUM_4
#define SENSOR_HUB_I2C_SCL_GPIO   GPIO_NUM_5
#define SENSOR_HUB_I2C_FREQ_HZ    100000  /* 100 kHz 标准模式 */
#define SENSOR_HUB_MQ2_ADC_GPIO   GPIO_NUM_1
#define SENSOR_HUB_AM312_GPIO     GPIO_NUM_6

/* MQ2 原始 ADC 值有效区间，超出则认为传感器未接或异常。 */
#define SENSOR_HUB_MQ2_MIN_VALID_RAW 10
#define SENSOR_HUB_MQ2_MAX_VALID_RAW 4085

/** 各传感器初始化成功/失败状态，供上层判断当前有哪些设备可用。 */
typedef struct {
    bool aht20;
    bool bmp280;
    bool bh1750;
    bool mq2;
    bool am312;
} sensor_hub_status_t;

/** 一次完整采集的传感器数据快照。 */
typedef struct {
    float aht_temperature;   /* AHT20 温度 (°C) */
    float humidity;          /* AHT20 相对湿度 (%) */
    float bmp_temperature;   /* BMP280 温度 (°C) */
    float pressure;          /* BMP280 气压 (Pa) */
    uint16_t lux;            /* BH1750 光照度 (lux) */
    int mq2_raw;             /* MQ2 ADC 原始值 */
    int mq2_voltage_mv;      /* MQ2 校准后电压 (mV)，校准不可用时为 -1 */
    int am312_raw_level;     /* AM312 GPIO 原始电平 */
    bool motion_detected;    /* AM312 人体活动检测结果（已按极性解释） */
    bool aht20_ok;           /* 本轮 AHT20 读取是否成功 */
    bool bmp280_ok;
    bool bh1750_ok;
    bool mq2_ok;
    bool am312_ok;
} sensor_hub_data_t;

/** 初始化所有传感器，单个失败不影响其他传感器。 */
esp_err_t SensorHub_Init(void);

/** 逐一读取各传感器数据，失败的传感器对应 _ok 字段为 false。 */
esp_err_t SensorHub_Read(sensor_hub_data_t *data);

/** 将传感器数据格式化输出到日志。 */
void SensorHub_LogData(const sensor_hub_data_t *data);

/** 便捷函数：读取 + 打印日志，用于调试。 */
void SensorHub_PollAndLog(void);

sensor_hub_status_t SensorHub_GetStatus(void);
