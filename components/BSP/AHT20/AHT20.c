/**
 * @file AHT20.c
 * @brief AHT20 温湿度传感器 BSP 驱动实现。
 */

#include "BSP_AHT20.h"

#include <string.h>
#include "BSP_I2C.h"
#include "aht.h"
#include "esp_log.h"

static const char *TAG = "BSP_AHT20";
static aht_t s_aht_dev = {0};    /* AHT 设备结构体 */
static bool s_initialized = false;

bool BSP_AHT20_IsInitialized(void)
{
    return s_initialized;
}

esp_err_t BSP_AHT20_Init(void)
{
    /* 重复初始化直接返回成功 */
    if (s_initialized) {
        return ESP_OK;
    }

    /* AHT20 依赖 I2C 总线，必须先初始化 I2C */
    if (!BSP_I2C_IsInitialized()) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 1. 初始化设备结构体：指定型号 AHT20，正常测量模式 */
    memset(&s_aht_dev, 0, sizeof(s_aht_dev));
    s_aht_dev.type = AHT_TYPE_AHT20;
    s_aht_dev.mode = AHT_MODE_NORMAL;

    /* 2. 创建 I2C 设备描述符 */
    esp_err_t ret = aht_init_desc(&s_aht_dev,
                                  AHT_I2C_ADDRESS_GND,   /* I2C 地址 0x38 */
                                  BSP_I2C_GetPort(),      /* 使用共享 I2C 端口 */
                                  BSP_I2C_GetSDA(),       /* 共享 SDA 引脚 */
                                  BSP_I2C_GetSCL());      /* 共享 SCL 引脚 */
    if (ret != ESP_OK) {
        return ret;
    }

    /* 3. 设置时钟频率，然后初始化传感器 */
    s_aht_dev.i2c_dev.cfg.master.clk_speed = BSP_I2C_GetClockSpeed();
    ret = aht_init(&s_aht_dev);
    if (ret != ESP_OK) {
        aht_free_desc(&s_aht_dev);   /* 初始化失败，释放描述符 */
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

    /* 调用库接口获取温湿度 */
    return aht_get_data(&s_aht_dev, temperature, humidity);
}
