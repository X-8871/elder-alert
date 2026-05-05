#include "BSP_BMP280.h"

#include <string.h>
#include "BSP_I2C.h"
#include "bmp280.h"
#include "esp_log.h"

static const char *TAG = "BSP_BMP280";
static bmp280_t s_bmp280_dev = {0};
static bool s_initialized = false;

bool BSP_BMP280_IsInitialized(void)
{
    return s_initialized;
}

esp_err_t BSP_BMP280_Init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    if (!BSP_I2C_IsInitialized()) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(&s_bmp280_dev, 0, sizeof(s_bmp280_dev));

    esp_err_t ret = bmp280_init_desc(&s_bmp280_dev,
                                     BMP280_I2C_ADDRESS_1,
                                     BSP_I2C_GetPort(),
                                     BSP_I2C_GetSDA(),
                                     BSP_I2C_GetSCL());
    if (ret != ESP_OK) {
        return ret;
    }

    s_bmp280_dev.i2c_dev.cfg.master.clk_speed = BSP_I2C_GetClockSpeed();

    bmp280_params_t params;
    ret = bmp280_init_default_params(&params);
    if (ret != ESP_OK) {
        bmp280_free_desc(&s_bmp280_dev);
        return ret;
    }

    ret = bmp280_init(&s_bmp280_dev, &params);
    if (ret != ESP_OK) {
        bmp280_free_desc(&s_bmp280_dev);
        return ret;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "init success, chip id=0x%02X", s_bmp280_dev.id);
    return ESP_OK;
}

esp_err_t BSP_BMP280_Read(float *temperature, float *pressure)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (temperature == NULL || pressure == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    return bmp280_read_float(&s_bmp280_dev, temperature, pressure, NULL);
}
