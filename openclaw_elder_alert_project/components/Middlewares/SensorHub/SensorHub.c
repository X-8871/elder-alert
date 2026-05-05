#include "SensorHub.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "BSP_AHT20.h"
#include "BSP_AM312.h"
#include "BSP_BH1750.h"
#include "BSP_BMP280.h"
#include "BSP_I2C.h"
#include "BSP_MQ2.h"

static const char *TAG = "SensorHub";
/* 记录各传感器初始化结果，供显示层和上层逻辑判断当前有哪些设备可用。 */
static sensor_hub_status_t s_sensor_ok = {0};
static bool s_initialized = false;

static void log_sensor_fault(const char *sensor_name, const char *stage, esp_err_t ret)
{
    ESP_LOGE(TAG, "FAULT: %s %s failed: %s", sensor_name, stage, esp_err_to_name(ret));
}

static void log_sensor_not_detected(const char *sensor_name)
{
    ESP_LOGW(TAG, "FAULT: %s not detected or not initialized", sensor_name);
}

static bool init_sensor(const char *sensor_name, esp_err_t (*init_fn)(void))
{
    /* 对 I2C 传感器统一复用这条初始化流程，减少重复代码。 */
    esp_err_t ret = init_fn();
    if (ret != ESP_OK) {
        log_sensor_fault(sensor_name, "init", ret);
        return false;
    }

    ESP_LOGI(TAG, "%s ready", sensor_name);
    return true;
}

static bool is_mq2_value_valid(const bsp_mq2_reading_t *reading)
{
    return reading != NULL &&
           reading->raw >= SENSOR_HUB_MQ2_MIN_VALID_RAW &&
           reading->raw <= SENSOR_HUB_MQ2_MAX_VALID_RAW;
}

esp_err_t SensorHub_Init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    /* 先把共享 I2C 总线准备好，AHT20/BMP280/BH1750/OLED 都会依赖它。 */
    esp_err_t ret = BSP_I2C_Init(SENSOR_HUB_I2C_PORT,
                                 SENSOR_HUB_I2C_SDA_GPIO,
                                 SENSOR_HUB_I2C_SCL_GPIO,
                                 SENSOR_HUB_I2C_FREQ_HZ);
    if (ret != ESP_OK) {
        log_sensor_fault("I2C", "init", ret);
        return ret;
    }

    /* 先初始化挂在 I2C 上的环境类传感器。 */
    s_sensor_ok.aht20 = init_sensor("AHT20", BSP_AHT20_Init);
    s_sensor_ok.bmp280 = init_sensor("BMP280", BSP_BMP280_Init);
    s_sensor_ok.bh1750 = init_sensor("BH1750", BSP_BH1750_Init);

    /* MQ2 走 ADC，不依赖 I2C。 */
    ret = BSP_MQ2_Init(SENSOR_HUB_MQ2_ADC_GPIO);
    if (ret == ESP_OK) {
        s_sensor_ok.mq2 = true;
        ESP_LOGI(TAG, "MQ2 ready");
    } else {
        log_sensor_fault("MQ2", "init", ret);
    }

    /* AM312 是纯 GPIO 输入，用于人体活动检测。 */
    ret = BSP_AM312_Init(SENSOR_HUB_AM312_GPIO, true);
    if (ret == ESP_OK) {
        s_sensor_ok.am312 = true;
        ESP_LOGI(TAG, "AM312 ready");
    } else {
        log_sensor_fault("AM312", "init", ret);
    }

    ESP_LOGW(TAG, "NOTE: MQ2/AM312 use ADC/GPIO; unplugged modules may need value-range checks or hardware detection");
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

    *data = (sensor_hub_data_t){0};
    bsp_mq2_reading_t mq2_reading = {0};
    esp_err_t ret = ESP_OK;

    /* 每个传感器各读各的；即使其中某个失败，也尽量保留其他传感器的结果。 */
    if (s_sensor_ok.aht20) {
        ret = BSP_AHT20_Read(&data->aht_temperature, &data->humidity);
        if (ret != ESP_OK) {
            log_sensor_fault("AHT20", "read", ret);
        }
    } else {
        log_sensor_not_detected("AHT20");
    }

    if (s_sensor_ok.bmp280) {
        ret = BSP_BMP280_Read(&data->bmp_temperature, &data->pressure);
        if (ret != ESP_OK) {
            log_sensor_fault("BMP280", "read", ret);
        }
    } else {
        log_sensor_not_detected("BMP280");
    }

    if (s_sensor_ok.bh1750) {
        ret = BSP_BH1750_Read(&data->lux);
        if (ret != ESP_OK) {
            log_sensor_fault("BH1750", "read", ret);
        }
    } else {
        log_sensor_not_detected("BH1750");
    }

    if (s_sensor_ok.mq2) {
        ret = BSP_MQ2_Read(&mq2_reading);
        if (ret == ESP_OK) {
            data->mq2_raw = mq2_reading.raw;
            data->mq2_voltage_mv = mq2_reading.voltage_mv;

            /* MQ2 没有天然“在线检测”能力，因此用原始值区间做一次合理性校验。 */
            if (!is_mq2_value_valid(&mq2_reading)) {
                ESP_LOGE(TAG,
                         "FAULT: MQ2 value out of valid range raw=%d valid=[%d,%d], check wiring/power",
                         data->mq2_raw,
                         SENSOR_HUB_MQ2_MIN_VALID_RAW,
                         SENSOR_HUB_MQ2_MAX_VALID_RAW);
            }
        } else {
            log_sensor_fault("MQ2", "read", ret);
        }
    } else {
        log_sensor_not_detected("MQ2");
    }

    if (s_sensor_ok.am312) {
        /* 先留原始电平，再给出已经按高低有效极性解释过的 motion_detected。 */
        ret = BSP_AM312_GetRawLevel(&data->am312_raw_level);
        if (ret != ESP_OK) {
            log_sensor_fault("AM312", "raw_read", ret);
        }

        ret = BSP_AM312_IsMotionDetected(&data->motion_detected);
        if (ret != ESP_OK) {
            log_sensor_fault("AM312", "read", ret);
        }
    } else {
        log_sensor_not_detected("AM312");
    }

    return ESP_OK;
}

void SensorHub_LogData(const sensor_hub_data_t *data)
{
    if (data == NULL) {
        ESP_LOGW(TAG, "SensorHub_LogData called with NULL data");
        return;
    }

    if (s_sensor_ok.aht20) {
        ESP_LOGI(TAG,
                 "AHT20: temperature=%.2f C humidity=%.2f %%",
                 data->aht_temperature,
                 data->humidity);
    }

    if (s_sensor_ok.bmp280) {
        ESP_LOGI(TAG,
                 "BMP280: temperature=%.2f C pressure=%.2f Pa",
                 data->bmp_temperature,
                 data->pressure);
    }

    if (s_sensor_ok.bh1750) {
        ESP_LOGI(TAG, "BH1750: illuminance=%u lux", data->lux);
    }

    if (s_sensor_ok.mq2) {
        ESP_LOGI(TAG,
                 "MQ2: raw=%d voltage=%d mV",
                 data->mq2_raw,
                 data->mq2_voltage_mv);
    }

    if (s_sensor_ok.am312) {
        ESP_LOGI(TAG, "AM312: motion_detected=%d", data->motion_detected);
    }
}
