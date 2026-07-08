/**
 * @file BH1750.c
 * @brief BH1750 光照度传感器 BSP 驱动实现。
 *
 * 【学弟必读：BH1750 底层】
 * 类似 AHT20，我们用了第三方的 `bh1750` 组件库，不需要自己操作 I2C 寄存器。
 * - `bh1750_init_desc()` 创建设备描述符
 * - `bh1750_setup()`  设置工作模式（连续高分辨率）
 * - `bh1750_read()`   读取 lux 值
 */

#include "BSP_BH1750.h"

#include <string.h>    /* memset */
#include "BSP_I2C.h"   /* 共享 I2C 总线 */
#include "bh1750.h"    /* 第三方 BH1750 驱动库 */
#include "esp_log.h"

static const char *TAG = "BSP_BH1750";
static i2c_dev_t s_bh1750_dev = {0};  /* 第三方 I2C 设备结构体 */
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

    /* 1. 创建 I2C 设备描述符：地址 0x23，挂载到共享 I2C 总线 */
    esp_err_t ret = bh1750_init_desc(&s_bh1750_dev,
                                     BH1750_ADDR_LO,         /* I2C 地址 0x23 */
                                     BSP_I2C_GetPort(),
                                     BSP_I2C_GetSDA(),
                                     BSP_I2C_GetSCL());
    if (ret != ESP_OK) {
        return ret;
    }

    /* 2. 设置传感器工作模式：连续 + 高分辨率 */
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
