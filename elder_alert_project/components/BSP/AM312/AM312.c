/**
 * @file AM312.c
 * @brief AM312 人体红外传感器 BSP 驱动实现，纯 GPIO 输入，支持高低电平有效极性配置。
 */

#include "BSP_AM312.h"

#include "esp_log.h"

static const char *TAG = "BSP_AM312";
static gpio_num_t s_signal_gpio = GPIO_NUM_NC;
static bool s_active_high = true;
static bool s_initialized = false;

bool BSP_AM312_IsInitialized(void)
{
    return s_initialized;
}

esp_err_t BSP_AM312_Init(gpio_num_t signal_gpio, bool active_high)
{
    if (signal_gpio < 0) {
        return ESP_ERR_INVALID_ARG;
    }

    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << signal_gpio,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        return ret;
    }

    s_signal_gpio = signal_gpio;
    s_active_high = active_high;
    s_initialized = true;

    ESP_LOGI(TAG, "init success on GPIO=%d active_high=%d", s_signal_gpio, s_active_high);
    return ESP_OK;
}

esp_err_t BSP_AM312_IsMotionDetected(bool *detected)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (detected == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    int level = gpio_get_level(s_signal_gpio);
    *detected = s_active_high ? (level != 0) : (level == 0);
    return ESP_OK;
}

esp_err_t BSP_AM312_GetRawLevel(int *level)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (level == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *level = gpio_get_level(s_signal_gpio);
    return ESP_OK;
}
