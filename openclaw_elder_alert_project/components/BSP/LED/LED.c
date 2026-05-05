#include "LED.h"
#include "driver/gpio.h"

#define LED_GPIO GPIO_NUM_4

static int s_led_level = 0;//用来记录当前 LED 输出状态

void LED_Init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    s_led_level = 0;
    gpio_set_level(LED_GPIO, s_led_level);
}

void LED_On(void)
{
    s_led_level = 1;
    gpio_set_level(LED_GPIO, s_led_level);
}

void LED_Off(void)
{
    s_led_level = 0;
    gpio_set_level(LED_GPIO, s_led_level);
}

void LED_Toggle(void)
{
    s_led_level = !s_led_level;
    gpio_set_level(LED_GPIO, s_led_level);
}
