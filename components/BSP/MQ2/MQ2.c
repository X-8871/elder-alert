/**
 * @file MQ2.c
 * @brief MQ2 烟雾/可燃气体传感器 BSP 驱动实现——ADC 单次采集 + 自动校准。
 */

#include "BSP_MQ2.h"

#include <stdbool.h>
#include "esp_log.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

static const char *TAG = "BSP_MQ2";
static adc_oneshot_unit_handle_t s_adc_handle = NULL;
static adc_cali_handle_t s_cali_handle = NULL;
static adc_channel_t s_adc_channel = ADC_CHANNEL_0;
static bool s_initialized = false;
static bool s_cali_enabled = false;

/**
 * @brief 将 GPIO 引脚号转换为对应的 ADC 通道编号。
 * 当前支持 GPIO1 ~ GPIO10 → ADC1_CH0 ~ ADC1_CH9
 */
static esp_err_t mq2_gpio_to_channel(gpio_num_t analog_gpio, adc_channel_t *channel)
{
    if (channel == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    switch (analog_gpio) {
    case GPIO_NUM_1:  *channel = ADC_CHANNEL_0; return ESP_OK;
    case GPIO_NUM_2:  *channel = ADC_CHANNEL_1; return ESP_OK;
    case GPIO_NUM_3:  *channel = ADC_CHANNEL_2; return ESP_OK;
    case GPIO_NUM_4:  *channel = ADC_CHANNEL_3; return ESP_OK;
    case GPIO_NUM_5:  *channel = ADC_CHANNEL_4; return ESP_OK;
    case GPIO_NUM_6:  *channel = ADC_CHANNEL_5; return ESP_OK;
    case GPIO_NUM_7:  *channel = ADC_CHANNEL_6; return ESP_OK;
    case GPIO_NUM_8:  *channel = ADC_CHANNEL_7; return ESP_OK;
    case GPIO_NUM_9:  *channel = ADC_CHANNEL_8; return ESP_OK;
    case GPIO_NUM_10: *channel = ADC_CHANNEL_9; return ESP_OK;
    default:           return ESP_ERR_INVALID_ARG;        /* 其他 GPIO 不支持 */
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

    /* 1. 把 GPIO 翻译为 ADC 通道号 */
    esp_err_t ret = mq2_gpio_to_channel(analog_gpio, &s_adc_channel);
    if (ret != ESP_OK) {
        return ret;
    }

    /* 2. 创建 ADC1 单元（oneshot 模式——每次手动触发一次转换） */
    adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = ADC_UNIT_1,  /* 使用 ADC1 */
    };
    ret = adc_oneshot_new_unit(&init_config, &s_adc_handle);
    if (ret != ESP_OK) {
        return ret;
    }

    /* 3. 配置 ADC 通道：默认位宽 + 12dB 衰减（量程扩展到 ~3.1V） */
    adc_oneshot_chan_cfg_t chan_config = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,  /* 默认 12-bit，范围 0~4095 */
        .atten = ADC_ATTEN_DB_12,          /* 12dB 衰减 */
    };
    ret = adc_oneshot_config_channel(s_adc_handle, s_adc_channel, &chan_config);
    if (ret != ESP_OK) {
        return ret;
    }

    /* 4. 尝试创建 ADC 校准句柄——优先曲线拟合，不支持则降级为线性拟合 */
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    /* 曲线拟合——更精确，需要芯片内置参考电压支持 */
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
    /* 线性拟合——较简单，兼容性更好 */
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

    /* 1. 读取 ADC 原始值 */
    int raw = 0;
    esp_err_t ret = adc_oneshot_read(s_adc_handle, s_adc_channel, &raw);
    if (ret != ESP_OK) {
        return ret;
    }

    reading->raw = raw;

    /* 2. 如果校准可用，把原始值转成电压 (mV) */
    if (s_cali_enabled) {
        ret = adc_cali_raw_to_voltage(s_cali_handle, raw, &reading->voltage_mv);
        if (ret != ESP_OK) {
            reading->voltage_mv = -1;  /* 转换失败，标记为无效 */
            return ret;
        }
    } else {
        reading->voltage_mv = -1;      /* 校准不可用 */
    }

    return ESP_OK;
}
