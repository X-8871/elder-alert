#include "BSP_BH1750.h"

#include <string.h>
#include "BSP_I2C.h"
#include "bh1750.h"
#include "esp_log.h"

static const char *TAG = "BSP_BH1750";
static i2c_dev_t s_bh1750_dev = {0};
static bool s_initialized = false;

bool BSP_BH1750_IsInitialized(void)
{
    return s_initialized;
}

esp_err_t BSP_BH1750_Init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    if (!BSP_I2C_IsInitialized()) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(&s_bh1750_dev, 0, sizeof(s_bh1750_dev));

    esp_err_t ret = bh1750_init_desc(&s_bh1750_dev,
                                     BH1750_ADDR_LO,
                                     BSP_I2C_GetPort(),
                                     BSP_I2C_GetSDA(),
                                     BSP_I2C_GetSCL());
    if (ret != ESP_OK) {
        return ret;
    }

    s_bh1750_dev.cfg.master.clk_speed = BSP_I2C_GetClockSpeed();
    ret = bh1750_setup(&s_bh1750_dev, BH1750_MODE_CONTINUOUS, BH1750_RES_HIGH);
    if (ret != ESP_OK) {
        bh1750_free_desc(&s_bh1750_dev);
        return ret;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "init success");
    return ESP_OK;
}

esp_err_t BSP_BH1750_Read(uint16_t *lux)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (lux == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    return bh1750_read(&s_bh1750_dev, lux);
}
