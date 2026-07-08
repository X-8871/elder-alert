/**
 * @file Alert.c
 * @brief LED + 蜂鸣器声光提示驱动实现——基于 FreeRTOS tick 的非阻塞节拍控制。
 *
 * 每次 Update() 检查当前时间与上次翻转时间的差值，
 * 达到对应模式的闪烁间隔时翻转 LED/蜂鸣器状态。
 */

#include "BSP_Alert.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

/* 不同模式的闪烁间隔 (ms) —— 越小闪烁越快，紧迫感越强 */
#define ALERT_NORMAL_BLINK_MS 1000   /* 1 秒一次 */
#define ALERT_REMIND_BLINK_MS 500    /* 0.5 秒一次 */
#define ALERT_ALARM_BLINK_MS  250    /* 0.25 秒一次 */
#define ALERT_SOS_BLINK_MS    120    /* 0.12 秒一次（非常急促） */

static const char *TAG = "BSP_Alert";
static gpio_num_t s_led_gpio = GPIO_NUM_NC;
static gpio_num_t s_buzzer_gpio = GPIO_NUM_NC;
static bsp_alert_mode_t s_mode = BSP_ALERT_MODE_OFF;
static TickType_t s_last_toggle_tick = 0;  /* 上次翻转的时刻（FreeRTOS tick） */
static bool s_led_on = false;
static bool s_buzzer_on = false;
static bool s_initialized = false;

/** 最底层的实际 GPIO 写入操作 */
static esp_err_t set_gpio_level(gpio_num_t gpio, bool on)
{
    return gpio_set_level(gpio, on ? 1 : 0);
}

/** 同时设置 LED 和蜂鸣器的输出状态并记录。
 *  蜂鸣器为低电平触发（active-low），输出电平需要取反。 */
static esp_err_t apply_outputs(bool led_on, bool buzzer_on)
{
    esp_err_t ret = set_gpio_level(s_led_gpio, led_on);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = set_gpio_level(s_buzzer_gpio, !buzzer_on);
    if (ret != ESP_OK) {
        return ret;
    }

    s_led_on = led_on;
    s_buzzer_on = buzzer_on;
    return ESP_OK;
}

bool BSP_Alert_IsInitialized(void)
{
    return s_initialized;
}

bsp_alert_mode_t BSP_Alert_GetMode(void)
{
    return s_mode;
}

esp_err_t BSP_Alert_Init(gpio_num_t led_gpio, gpio_num_t buzzer_gpio)
{
    /* 不允许 LED 和蜂鸣器用同一个引脚 */
    if (led_gpio < 0 || buzzer_gpio < 0 || led_gpio == buzzer_gpio) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 配置两个引脚为输出模式 */
    uint64_t pin_mask = (1ULL << led_gpio) | (1ULL << buzzer_gpio);
    gpio_config_t io_conf = {
        .pin_bit_mask = pin_mask,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        return ret;
    }

    s_led_gpio = led_gpio;
    s_buzzer_gpio = buzzer_gpio;
    s_mode = BSP_ALERT_MODE_OFF;
    s_last_toggle_tick = xTaskGetTickCount();
    s_initialized = true;

    /* 初始状态：全关 */
    ret = apply_outputs(false, false);
    if (ret != ESP_OK) {
        return ret;
    }

    ESP_LOGI(TAG, "init success: led_gpio=%d buzzer_gpio=%d", s_led_gpio, s_buzzer_gpio);
    return ESP_OK;
}

esp_err_t BSP_Alert_SetMode(bsp_alert_mode_t mode)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (mode == s_mode) {
        return ESP_OK;  /* 模式没变，不需要操作 */
    }

    /* 切换模式时：重置节拍起点，让新模式立即生效 */
    s_mode = mode;
    s_last_toggle_tick = xTaskGetTickCount();

    /* 设置新模式的初始输出状态 */
    switch (s_mode) {
    case BSP_ALERT_MODE_OFF:
        return apply_outputs(false, false);     /* 全关 */
    case BSP_ALERT_MODE_NORMAL:
        return apply_outputs(true, false);      /* LED 常亮，蜂鸣器关 */
    case BSP_ALERT_MODE_REMIND:
        return apply_outputs(true, false);      /* 初始点亮，之后 Update 中翻转 */
    case BSP_ALERT_MODE_ALARM:
        return apply_outputs(true, true);       /* 初始 LED 亮 + 蜂鸣器响 */
    case BSP_ALERT_MODE_SOS:
        return apply_outputs(true, true);
    default:
        return ESP_ERR_INVALID_ARG;
    }
}

esp_err_t BSP_Alert_SetOutputs(bool led_on, bool buzzer_on)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    s_mode = BSP_ALERT_MODE_OFF;  /* 直接控制模式视为退出闪烁模式 */
    return apply_outputs(led_on, buzzer_on);
}

esp_err_t BSP_Alert_Update(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    TickType_t now = xTaskGetTickCount();
    TickType_t interval = 0;

    switch (s_mode) {
    case BSP_ALERT_MODE_OFF:
        /* OFF 模式：保持全关 */
        return apply_outputs(false, false);

    case BSP_ALERT_MODE_NORMAL:
        /* NORMAL：LED 按 1000ms 节拍闪烁 */
        interval = pdMS_TO_TICKS(ALERT_NORMAL_BLINK_MS);
        if ((now - s_last_toggle_tick) >= interval) {
            s_last_toggle_tick = now;
            return apply_outputs(!s_led_on, false);  /* 只翻转 LED，蜂鸣器始终关 */
        }
        return ESP_OK;

    case BSP_ALERT_MODE_REMIND:
        /* REMIND：LED + 蜂鸣器按 500ms 节拍同步翻转 */
        interval = pdMS_TO_TICKS(ALERT_REMIND_BLINK_MS);
        if ((now - s_last_toggle_tick) >= interval) {
            s_last_toggle_tick = now;
            return apply_outputs(!s_led_on, !s_led_on);
        }
        return ESP_OK;

    case BSP_ALERT_MODE_ALARM:
        /* ALARM：LED + 蜂鸣器按 250ms 节拍 */
        interval = pdMS_TO_TICKS(ALERT_ALARM_BLINK_MS);
        if ((now - s_last_toggle_tick) >= interval) {
            s_last_toggle_tick = now;
            return apply_outputs(!s_led_on, !s_buzzer_on);
        }
        return ESP_OK;

    case BSP_ALERT_MODE_SOS:
        /* SOS：LED + 蜂鸣器按 120ms 节拍（最高频，最紧迫） */
        interval = pdMS_TO_TICKS(ALERT_SOS_BLINK_MS);
        if ((now - s_last_toggle_tick) >= interval) {
            s_last_toggle_tick = now;
            return apply_outputs(!s_led_on, !s_buzzer_on);
        }
        return ESP_OK;

    default:
        return ESP_ERR_INVALID_ARG;
    }
}
