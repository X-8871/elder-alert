/**
 * @file SensorHub.c
 * @brief 传感器中枢实现——统一初始化和采集 AHT20/BH1750/MQ2/LD2410B。
 *
 * 【学弟必读：SensorHub 的设计思路】
 * SensorHub 是"传感器管理者"，不是"传感器本身"。
 * 它不直接操作 I2C/ADC/UART 寄存器，而是调用各个 BSP 驱动模块。
 *
 * 关键设计决策：
 * 1. **独立初始化**：每个传感器单独初始化，AHT20 坏了不影响 BH1750 工作。
 * 2. **各自读取**：Read() 中每个传感器独立读取，一个失败不丢其他传感器的数据。
 * 3. **_ok 标记**：每帧数据都有对应的 _ok 字段，上层根据这个判断"这个值能不能用"。
 * 4. **MQ2 合理性校验**：因为 MQ2 没有"在线/离线"的硬件检测，所以通过"原始值是否在合理区间内"
 *    来推断传感器是否接了、是否有故障。
 */

#include "SensorHub.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "BSP_AHT20.h"
#include "BSP_BH1750.h"
#include "BSP_I2C.h"
#include "BSP_LD2410B.h"
#include "BSP_MQ2.h"

static const char *TAG = "SensorHub";
static sensor_hub_status_t s_sensor_ok = {0};  /* 各传感器是否初始化成功 */
static bool s_initialized = false;

/* ---- 辅助日志函数：减少重复代码 ---- */

static void log_sensor_fault(const char *sensor_name, const char *stage, esp_err_t ret)
{
    ESP_LOGE(TAG, "FAULT: %s %s failed: %s", sensor_name, stage, esp_err_to_name(ret));
}

static void log_sensor_not_detected(const char *sensor_name)
{
    ESP_LOGW(TAG, "FAULT: %s not detected or not initialized", sensor_name);
}

/** I2C 传感器的初始化模板——统一复用，减少重复代码 */
static bool init_sensor(const char *sensor_name, esp_err_t (*init_fn)(void))
{
    esp_err_t ret = init_fn();
    if (ret != ESP_OK) {
        log_sensor_fault(sensor_name, "init", ret);
        return false;
    }

    ESP_LOGI(TAG, "%s ready", sensor_name);
    return true;
}

/** MQ2 没有在线检测引脚，通过原始值是否在合理区间内来推断是否接了传感器 */
static bool is_mq2_value_valid(const bsp_mq2_reading_t *reading)
{
    return reading != NULL &&
           reading->raw >= SENSOR_HUB_MQ2_MIN_VALID_RAW &&
           reading->raw <= SENSOR_HUB_MQ2_MAX_VALID_RAW;
}

/* ================================================================
 * 公开接口
 * ================================================================ */

esp_err_t SensorHub_Init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    /*
     * 第一步：先初始化共享 I2C 总线。
     * AHT20、BH1750、OLED 都挂在这条总线上，所以必须先做。
     * 本项目 I2C 使用 GPIO4(SDA) + GPIO5(SCL)，100kHz 标准模式。
     */
    esp_err_t ret = BSP_I2C_Init(SENSOR_HUB_I2C_PORT,
                                 SENSOR_HUB_I2C_SDA_GPIO,
                                 SENSOR_HUB_I2C_SCL_GPIO,
                                 SENSOR_HUB_I2C_FREQ_HZ);
    if (ret != ESP_OK) {
        log_sensor_fault("I2C", "init", ret);
        return ret;
    }

    /* 第二步：初始化挂在 I2C 上的环境传感器 */
    s_sensor_ok.aht20 = init_sensor("AHT20", BSP_AHT20_Init);
    s_sensor_ok.bh1750 = init_sensor("BH1750", BSP_BH1750_Init);

    /* 第三步：MQ2 走 ADC1_CH0(GPIO1)，不依赖 I2C */
    ret = BSP_MQ2_Init(SENSOR_HUB_MQ2_ADC_GPIO);
    if (ret == ESP_OK) {
        s_sensor_ok.mq2 = true;
        ESP_LOGI(TAG, "MQ2 ready");
    } else {
        log_sensor_fault("MQ2", "init", ret);
    }

    /* 第四步：LD2410B 走 UART1(GPIO18=TX, GPIO16=RX)，波特率 256000 */
    const bsp_ld2410b_config_t ld2410b_config = {
        .uart_num = SENSOR_HUB_LD2410B_UART_NUM,
        .tx_gpio = SENSOR_HUB_LD2410B_TX_GPIO,
        .rx_gpio = SENSOR_HUB_LD2410B_RX_GPIO,
        .baud_rate = BSP_LD2410B_DEFAULT_BAUD_RATE,
    };
    ret = BSP_LD2410B_Init(&ld2410b_config);
    if (ret == ESP_OK) {
        s_sensor_ok.ld2410b = true;
        ESP_LOGI(TAG, "LD2410B ready");
    } else {
        log_sensor_fault("LD2410B", "init", ret);
    }

    ESP_LOGW(TAG, "NOTE: MQ2 uses ADC; unplugged modules may need value-range checks or hardware detection");
    s_initialized = true;
    return ESP_OK;
}

sensor_hub_status_t SensorHub_GetStatus(void)
{
    return s_sensor_ok;
}

void SensorHub_PollAndLog(void)
{
    sensor_hub_data_t data = {0};
    esp_err_t ret = SensorHub_Read(&data);
    if (ret == ESP_OK) {
        SensorHub_LogData(&data);
    } else {
        log_sensor_fault("SensorHub", "read", ret);
    }
}

esp_err_t SensorHub_Read(sensor_hub_data_t *data)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 先清零，避免残留上轮数据 */
    *data = (sensor_hub_data_t){0};
    bsp_mq2_reading_t mq2_reading = {0};
    esp_err_t ret = ESP_OK;

    /* ------ AHT20：温度 + 湿度 ------ */
    if (s_sensor_ok.aht20) {
        ret = BSP_AHT20_Read(&data->aht_temperature, &data->humidity);
        if (ret != ESP_OK) {
            log_sensor_fault("AHT20", "read", ret);
        } else {
            data->aht20_ok = true;
        }
    } else {
        log_sensor_not_detected("AHT20");
    }

    /* ------ BH1750：光照度 ------ */
    if (s_sensor_ok.bh1750) {
        ret = BSP_BH1750_Read(&data->lux);
        if (ret != ESP_OK) {
            log_sensor_fault("BH1750", "read", ret);
        } else {
            data->bh1750_ok = true;
        }
    } else {
        log_sensor_not_detected("BH1750");
    }

    /* ------ MQ2：ADC 原始值 + 电压 ------ */
    if (s_sensor_ok.mq2) {
        ret = BSP_MQ2_Read(&mq2_reading);
        if (ret == ESP_OK) {
            data->mq2_raw = mq2_reading.raw;
            data->mq2_voltage_mv = mq2_reading.voltage_mv;

            if (!is_mq2_value_valid(&mq2_reading)) {
                ESP_LOGE(TAG,
                         "FAULT: MQ2 value out of valid range raw=%d valid=[%d,%d], check wiring/power",
                         data->mq2_raw,
                         SENSOR_HUB_MQ2_MIN_VALID_RAW,
                         SENSOR_HUB_MQ2_MAX_VALID_RAW);
            } else {
                data->mq2_ok = true;
            }
        } else {
            log_sensor_fault("MQ2", "read", ret);
        }
    } else {
        log_sensor_not_detected("MQ2");
    }

    /* ------ LD2410B：毫米波人体存在/运动检测 ------ */
    if (s_sensor_ok.ld2410b) {
        bsp_ld2410b_status_t status = {0};
        ret = BSP_LD2410B_ReadStatus(&status, SENSOR_HUB_LD2410B_READ_TIMEOUT_MS);
        if (ret == ESP_OK) {
            data->motion_detected = status.moving_target;
            data->ld2410b_presence = status.presence;
            data->ld2410b_moving_target = status.moving_target;
            data->ld2410b_stationary_target = status.stationary_target;
            data->ld2410b_moving_distance_cm = status.moving_distance_cm;
            data->ld2410b_moving_energy = status.moving_energy;
            data->ld2410b_stationary_distance_cm = status.stationary_distance_cm;
            data->ld2410b_stationary_energy = status.stationary_energy;
            data->ld2410b_detection_distance_cm = status.detection_distance_cm;
            data->ld2410b_ok = true;
        } else if (ret != ESP_ERR_TIMEOUT) {
            /* 超时不算是错误——模块可能暂时没有人进入探测区 */
            log_sensor_fault("LD2410B", "read", ret);
        }
    } else {
        log_sensor_not_detected("LD2410B");
    }

    return ESP_OK;
}

void SensorHub_LogData(const sensor_hub_data_t *data)
{
    if (data == NULL) {
        ESP_LOGW(TAG, "SensorHub_LogData called with NULL data");
        return;
    }

    if (data->aht20_ok) {
        ESP_LOGI(TAG, "AHT20: temperature=%.2f C humidity=%.2f %%",
                 data->aht_temperature, data->humidity);
    }

    if (data->bh1750_ok) {
        ESP_LOGI(TAG, "BH1750: illuminance=%u lux", data->lux);
    }

    if (data->mq2_ok) {
        ESP_LOGI(TAG, "MQ2: raw=%d voltage=%d mV", data->mq2_raw, data->mq2_voltage_mv);
    }

    if (data->ld2410b_ok) {
        ESP_LOGI(TAG,
                 "LD2410B: presence=%d moving=%d stationary=%d moving_cm=%u stationary_cm=%u detect_cm=%u",
                 data->ld2410b_presence, data->ld2410b_moving_target,
                 data->ld2410b_stationary_target,
                 (unsigned)data->ld2410b_moving_distance_cm,
                 (unsigned)data->ld2410b_stationary_distance_cm,
                 (unsigned)data->ld2410b_detection_distance_cm);
    }
}
