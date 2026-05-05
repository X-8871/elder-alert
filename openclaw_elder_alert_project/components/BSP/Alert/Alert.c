#include "BSP_Alert.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#define ALERT_NORMAL_BLINK_MS 1000
#define ALERT_REMIND_BLINK_MS 500
#define ALERT_ALARM_BLINK_MS  250
#define ALERT_SOS_BLINK_MS    120

static const char *TAG = "BSP_Alert";
static gpio_num_t s_led_gpio = GPIO_NUM_NC;
static gpio_num_t s_buzzer_gpio = GPIO_NUM_NC;
static bsp_alert_mode_t s_mode = BSP_ALERT_MODE_OFF;
static TickType_t s_last_toggle_tick = 0;
static bool s_led_on = false;
static bool s_buzzer_on = false;
static bool s_initialized = false;

static esp_err_t set_gpio_level(gpio_num_t gpio, bool on)
{
    return gpio_set_level(gpio, on ? 1 : 0);
}

static esp_err_t apply_outputs(bool led_on, bool buzzer_on)
{
    /* 这是最底层的实际落地动作：把目标状态真正写到 GPIO。 */
    esp_err_t ret = set_gpio_level(s_led_gpio, led_on);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = set_gpio_level(s_buzzer_gpio, buzzer_on);
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
    if (led_gpio < 0 || buzzer_gpio < 0 || led_gpio == buzzer_gpio) {
        return ESP_ERR_INVALID_ARG;
    }

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
        return ESP_OK;
    }

    /* 切模式时重置节拍基准，让新模式从“当前时刻”重新开始。 */
    s_mode = mode;
    s_last_toggle_tick = xTaskGetTickCount();

    switch (s_mode) {
    case BSP_ALERT_MODE_OFF:
        return apply_outputs(false, false);
    case BSP_ALERT_MODE_NORMAL:
        return apply_outputs(true, false);
    case BSP_ALERT_MODE_REMIND:
        return apply_outputs(true, false);
    case BSP_ALERT_MODE_ALARM:
        return apply_outputs(true, true);
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

    s_mode = BSP_ALERT_MODE_OFF;
    return apply_outputs(led_on, buzzer_on);
}

esp_err_t BSP_Alert_Update(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    TickType_t now = xTaskGetTickCount();
    TickType_t interval = 0;

    /* Update 只负责按当前模式推进闪烁/鸣叫节拍。 */
    switch (s_mode) {
    case BSP_ALERT_MODE_OFF:
        return apply_outputs(false, false);
    case BSP_ALERT_MODE_NORMAL:
        interval = pdMS_TO_TICKS(ALERT_NORMAL_BLINK_MS);
        if ((now - s_last_toggle_tick) >= interval) {
            s_last_toggle_tick = now;
            return apply_outputs(!s_led_on, false);
        }
        return ESP_OK;
    case BSP_ALERT_MODE_REMIND:
        interval = pdMS_TO_TICKS(ALERT_REMIND_BLINK_MS);
        if ((now - s_last_toggle_tick) >= interval) {
            s_last_toggle_tick = now;
            return apply_outputs(!s_led_on, !s_led_on);
        }
        return ESP_OK;
    case BSP_ALERT_MODE_ALARM:
        interval = pdMS_TO_TICKS(ALERT_ALARM_BLINK_MS);
        if ((now - s_last_toggle_tick) >= interval) {
            s_last_toggle_tick = now;
            return apply_outputs(!s_led_on, !s_buzzer_on);
        }
        return ESP_OK;
    case BSP_ALERT_MODE_SOS:
        interval = pdMS_TO_TICKS(ALERT_SOS_BLINK_MS);
        if ((now - s_last_toggle_tick) >= interval) {
            s_last_toggle_tick = now;
            return apply_outputs(!s_led_on, true);
        }
        return ESP_OK;
    default:
        return ESP_ERR_INVALID_ARG;
    }
}
