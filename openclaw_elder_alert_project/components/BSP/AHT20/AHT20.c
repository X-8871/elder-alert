/**
 * @file AHT20.c
 * @brief AHT20 温湿度传感器 BSP 驱动实现，依赖共享 I2C 总线。
 */

#include "BSP_AHT20.h"

#include <string.h>
#include "BSP_I2C.h"
#include "aht.h"
#include "esp_log.h"

static const char *TAG = "BSP_AHT20";
static aht_t s_aht_dev = {0};
static bool s_initialized = false;

bool BSP_AHT20_IsInitialized(void)
{
    return s_initialized;
}

esp_err_t BSP_AHT20_Init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    if (!BSP_I2C_IsInitialized()) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(&s_aht_dev, 0, sizeof(s_aht_dev));
    s_aht_dev.type = AHT_TYPE_AHT20;
    s_aht_dev.mode = AHT_MODE_NORMAL;

    esp_err_t ret = aht_init_desc(&s_aht_dev,
                                  AHT_I2C_ADDRESS_GND,
                                  BSP_I2C_GetPort(),
                                  BSP_I2C_GetSDA(),
                                  BSP_I2C_GetSCL());
    if (ret != ESP_OK) {
        return ret;
    }

    s_aht_dev.i2c_dev.cfg.master.clk_speed = BSP_I2C_GetClockSpeed();
    ret = aht_init(&s_aht_dev);
    if (ret != ESP_OK) {
        aht_free_desc(&s_aht_dev);
        return ret;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "init success");
    return ESP_OK;
}

esp_err_t BSP_AHT20_Read(float *temperature, float *humidity)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (temperature == NULL || humidity == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    return aht_get_data(&s_aht_dev, temperature, humidity);
}
