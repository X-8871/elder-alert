/**
 * @file BSP_I2C.c
 * @brief 共享 I2C 总线管理实现——全局单例模式，底层委托给 i2cdev 组件完成实际的寄存器操作。
 *
 * 【学弟必读：什么是"单例模式"？】
 * 整个系统中只需要一条 I2C 总线（连接 AHT20/BH1750/OLED 等），
 * 所以这里的静态变量（s_initialized / s_port / ...）全局只保存一份配置，
 * 保证不会被重复初始化出两条不同的 I2C 总线。
 */

#include "BSP_I2C.h"

#include "i2cdev.h"     /* 第三方 i2cdev 库——封装了 ESP-IDF I2C 驱动的更友好接口 */
#include "esp_log.h"    /* ESP-IDF 日志宏：ESP_LOGI / ESP_LOGW / ESP_LOGE */

static const char *TAG = "BSP_I2C";  /* 日志标签，串口输出时会显示在每行前面 */
static bool s_initialized = false;    /* 是否已完成初始化 */

/* ---- 保存当前共享 I2C 总线的唯一配置 ---- */
static i2c_port_t s_port = I2C_NUM_MAX;       /* I2C 控制器编号 (0 或 1) */
static gpio_num_t s_sda_io_num = GPIO_NUM_NC; /* SDA 数据引脚 */
static gpio_num_t s_scl_io_num = GPIO_NUM_NC; /* SCL 时钟引脚 */
static uint32_t s_clk_speed_hz = 0;            /* 总线时钟频率 */

esp_err_t BSP_I2C_Init(i2c_port_t port, gpio_num_t sda_io_num, gpio_num_t scl_io_num, uint32_t clk_speed_hz)
{
    /* 1. 参数合法性检查 */
    if (port >= I2C_NUM_MAX || sda_io_num < 0 || scl_io_num < 0 || clk_speed_hz == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 2. 如果已初始化，检查配置是否完全一致 */
    if (s_initialized) {
        if (s_port == port && s_sda_io_num == sda_io_num &&
            s_scl_io_num == scl_io_num && s_clk_speed_hz == clk_speed_hz) {
            return ESP_OK;  /* 相同配置，无需重复初始化 */
        }
        ESP_LOGW(TAG, "already initialized on port=%d SDA=%d SCL=%d", s_port, s_sda_io_num, s_scl_io_num);
        return ESP_ERR_INVALID_STATE;  /* 不同配置，拒绝 */
    }

    /* 3. 调用 i2cdev 库完成底层初始化（安装驱动、配置时钟等） */
    esp_err_t ret = i2cdev_init();
    if (ret != ESP_OK) {
        return ret;
    }

    /* 4. 保存配置到静态变量，标记初始化完成 */
    s_initialized = true;
    s_port = port;
    s_sda_io_num = sda_io_num;
    s_scl_io_num = scl_io_num;
    s_clk_speed_hz = clk_speed_hz;

    ESP_LOGI(TAG, "init success: port=%d SDA=%d SCL=%d FREQ=%lu",
             s_port, s_sda_io_num, s_scl_io_num, (unsigned long)s_clk_speed_hz);
    return ESP_OK;
}

/* ---- 以下都是简单的状态查询函数，直接返回静态变量即可 ---- */

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
