/**
 * @file MQ2.c
 * @brief MQ2 烟雾/可燃气体传感器 BSP 驱动实现，ADC 单次采集 + 校准。
 *
 * GPIO 到 ADC 通道的映射支持 GPIO1~GPIO10，校准方案自动适配
 * curve fitting 或 line fitting（取决于芯片支持）。
 */

#include "BSP_MQ2.h"

#include <stdbool.h>
#include "esp_log.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

static const char *TAG = "BSP_MQ2";
static adc_oneshot_unit_handle_t s_adc_handle = NULL;// ADC 单元句柄
static adc_cali_handle_t s_cali_handle = NULL;//ADC校准句柄
static adc_channel_t s_adc_channel = ADC_CHANNEL_0;
static bool s_initialized = false;
static bool s_cali_enabled = false;

static esp_err_t mq2_gpio_to_channel(gpio_num_t analog_gpio, adc_channel_t *channel)//将 MQ2 使用的 GPIO 转换为 ADC 通道
{
    if (channel == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    switch (analog_gpio) {
    case GPIO_NUM_1:
        *channel = ADC_CHANNEL_0;
        return ESP_OK;
    case GPIO_NUM_2:
        *channel = ADC_CHANNEL_1;
        return ESP_OK;
    case GPIO_NUM_3:
        *channel = ADC_CHANNEL_2;
        return ESP_OK;
    case GPIO_NUM_4:
        *channel = ADC_CHANNEL_3;
        return ESP_OK;
    case GPIO_NUM_5:
        *channel = ADC_CHANNEL_4;
        return ESP_OK;
    case GPIO_NUM_6:
        *channel = ADC_CHANNEL_5;
        return ESP_OK;
    case GPIO_NUM_7:
        *channel = ADC_CHANNEL_6;
        return ESP_OK;
    case GPIO_NUM_8:
        *channel = ADC_CHANNEL_7;
        return ESP_OK;
    case GPIO_NUM_9:
        *channel = ADC_CHANNEL_8;
        return ESP_OK;
    case GPIO_NUM_10:
        *channel = ADC_CHANNEL_9;
        return ESP_OK;
    default:
        return ESP_ERR_INVALID_ARG;
    }
}

bool BSP_MQ2_IsInitialized(void)
{
    return s_initialized;
}

esp_err_t BSP_MQ2_Init(gpio_num_t analog_gpio)
{
    if (s_initialized) {
        return ESP_OK;
    }

    esp_err_t ret = mq2_gpio_to_channel(analog_gpio, &s_adc_channel);
    if (ret != ESP_OK) {
        return ret;
    }

    adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = ADC_UNIT_1,
    };
    ret = adc_oneshot_new_unit(&init_config, &s_adc_handle);
    if (ret != ESP_OK) {
        return ret;
    }

    adc_oneshot_chan_cfg_t chan_config = { //配置 ADC 通道
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten = ADC_ATTEN_DB_12,
    };
    ret = adc_oneshot_config_channel(s_adc_handle, s_adc_channel, &chan_config);//应用 ADC 通道配置
    if (ret != ESP_OK) {
        return ret;
    }
    // 尝试创建 ADC 校准句柄，用于将 raw 转换为电压 mV
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cali_config = {
        .unit_id = ADC_UNIT_1,
        .chan = s_adc_channel,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali_config, &s_cali_handle) == ESP_OK) {
        s_cali_enabled = true;
    }
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_line_fitting_config_t cali_config = {
        .unit_id = ADC_UNIT_1,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_line_fitting(&cali_config, &s_cali_handle) == ESP_OK) {
        s_cali_enabled = true;
    }
#endif

    s_initialized = true;
    ESP_LOGI(TAG, "init success on GPIO=%d channel=%d", analog_gpio, s_adc_channel);
    return ESP_OK;
}

esp_err_t BSP_MQ2_Read(bsp_mq2_reading_t *reading)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (reading == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    int raw = 0;
    esp_err_t ret = adc_oneshot_read(s_adc_handle, s_adc_channel, &raw);
    if (ret != ESP_OK) {
        return ret;
    }

    reading->raw = raw;
    if (s_cali_enabled) //如果校准可用，把 raw 转成 voltage_mv
    {
        ret = adc_cali_raw_to_voltage(s_cali_handle, raw, &reading->voltage_mv);
        if (ret != ESP_OK) {
            reading->voltage_mv = -1;
            return ret;
        }
    } else {
        reading->voltage_mv = -1;
    }

    return ESP_OK;
}
