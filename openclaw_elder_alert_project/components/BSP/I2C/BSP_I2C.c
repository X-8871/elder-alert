/**
 * @file BSP_I2C.c
 * @brief 共享 I2C 总线管理实现，全局单例，底层委托给 i2cdev 组件。
 */

#include "BSP_I2C.h"

#include "i2cdev.h"
#include "esp_log.h"

static const char *TAG = "BSP_I2C";
static bool s_initialized = false;

/* 这几个静态变量保存“当前这条共享 I2C 总线”的唯一配置。 */
static i2c_port_t s_port = I2C_NUM_MAX;
static gpio_num_t s_sda_io_num = GPIO_NUM_NC;
static gpio_num_t s_scl_io_num = GPIO_NUM_NC;
static uint32_t s_clk_speed_hz = 0;

esp_err_t BSP_I2C_Init(i2c_port_t port, gpio_num_t sda_io_num, gpio_num_t scl_io_num, uint32_t clk_speed_hz)
{
    if (port >= I2C_NUM_MAX || sda_io_num < 0 || scl_io_num < 0 || clk_speed_hz == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_initialized) {
        /* 如果配置完全相同，就认为已经初始化完成；否则拒绝重复初始化不同配置。 */
        if (s_port == port && s_sda_io_num == sda_io_num && s_scl_io_num == scl_io_num && s_clk_speed_hz == clk_speed_hz) {
            return ESP_OK;
        }
        ESP_LOGW(TAG, "already initialized on port=%d SDA=%d SCL=%d", s_port, s_sda_io_num, s_scl_io_num);
        return ESP_ERR_INVALID_STATE;
    }

    /* 实际底层初始化交给第三方 i2cdev 组件。 */
    esp_err_t ret = i2cdev_init();
    if (ret != ESP_OK) {
        return ret;
    }

    s_initialized = true;
    s_port = port;
    s_sda_io_num = sda_io_num;
    s_scl_io_num = scl_io_num;
    s_clk_speed_hz = clk_speed_hz;

    ESP_LOGI(TAG, "init success: port=%d SDA=%d SCL=%d FREQ=%lu",
             s_port, s_sda_io_num, s_scl_io_num, (unsigned long)s_clk_speed_hz);
    return ESP_OK;
}

bool BSP_I2C_IsInitialized(void)
{
    return s_initialized;
}

i2c_port_t BSP_I2C_GetPort(void)
{
    return s_port;
}

gpio_num_t BSP_I2C_GetSDA(void)
{
    return s_sda_io_num;
}

gpio_num_t BSP_I2C_GetSCL(void)
{
    return s_scl_io_num;
}

uint32_t BSP_I2C_GetClockSpeed(void)
{
    return s_clk_speed_hz;
}
