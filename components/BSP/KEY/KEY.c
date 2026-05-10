/**
 * @file KEY.c
 * @brief 按键驱动实现，包含确认键、SOS 键和录音键检测机制。
 *
 * GPIO7（确认键）：轮询方式，20ms 消抖窗口，状态变化时返回事件。
 * GPIO8（SOS 键）：双沿中断触发，消抖后判断按下/松开。
 * GPIO17（录音键）：双沿中断触发，消抖后判断按下/松开。
 */

#include "KEY.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"

#define KEY_GPIO GPIO_NUM_7
#define KEY_EXTI_GPIO GPIO_NUM_8
#define KEY_RECORD_GPIO GPIO_NUM_17
#define KEY_DEBOUNCE_MS 20

static int s_key_stable_level = 1;
static int s_key_last_read_level = 1;

static int s_exti_stable_level = 1;
static volatile int s_exti_irq_pending = 0;
static int s_record_stable_level = 1;
static volatile int s_record_irq_pending = 0;
static int s_isr_service_installed = 0;

static void IRAM_ATTR key_irq_handler(void *arg)
{
    volatile int *pending = (volatile int *)arg;
    if (pending != NULL) {
        *pending = 1;
    }
}

void KEY_Init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << KEY_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    s_key_stable_level = gpio_get_level(KEY_GPIO);
    s_key_last_read_level = s_key_stable_level;
}

/* 低电平有效，按下返回 1，未按下返回 0。 */
int KEY_IsPressed(void)
{
    return gpio_get_level(KEY_GPIO) == 0;
}

/* 轮询确认键：带消抖，只在稳定状态发生变化时返回 1。 */
int KEY_Scan(int *is_pressed)
{
    int current_level = gpio_get_level(KEY_GPIO);

    if (current_level == s_key_last_read_level) {
        return 0;
    }

    vTaskDelay(pdMS_TO_TICKS(KEY_DEBOUNCE_MS));
    current_level = gpio_get_level(KEY_GPIO);
    if (current_level != s_key_last_read_level) {
        s_key_last_read_level = current_level;

        if (current_level != s_key_stable_level) {
            s_key_stable_level = current_level;

            if (is_pressed != NULL) {
                *is_pressed = (s_key_stable_level == 0);
            }
            return 1;
        }
    }

    return 0;
}

void KEY_EXTI_Init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << KEY_EXTI_GPIO) | (1ULL << KEY_RECORD_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    gpio_config(&io_conf);

    s_exti_stable_level = gpio_get_level(KEY_EXTI_GPIO);
    s_exti_irq_pending = 0;
    s_record_stable_level = gpio_get_level(KEY_RECORD_GPIO);
    s_record_irq_pending = 0;

    if (!s_isr_service_installed) {
        /* GPIO 中断服务全局只需要安装一次。 */
        gpio_install_isr_service(0);
        s_isr_service_installed = 1;
    }

    /* 把 SOS 键和录音键分别绑定到中断回调。 */
    gpio_isr_handler_add(KEY_EXTI_GPIO, key_irq_handler, (void *)&s_exti_irq_pending);
    gpio_isr_handler_add(KEY_RECORD_GPIO, key_irq_handler, (void *)&s_record_irq_pending);
}

int KEY_EXTI_IsPressed(void)
{
    return gpio_get_level(KEY_EXTI_GPIO) == 0;
}

static int get_interrupt_key_event(gpio_num_t gpio_num,
                                   volatile int *irq_pending,
                                   int *stable_level,
                                   int *is_pressed)
{
    int current_level = 0;

    if (irq_pending == NULL || stable_level == NULL || !(*irq_pending)) {
        return 0;
    }

    /* 临时关中断，避免一次按键抖动触发出多次事件。 */
    gpio_intr_disable(gpio_num);
    *irq_pending = 0;

    vTaskDelay(pdMS_TO_TICKS(KEY_DEBOUNCE_MS));
    current_level = gpio_get_level(gpio_num);
    if (current_level != *stable_level) {
        *stable_level = current_level;

        if (is_pressed != NULL) {
            *is_pressed = (*stable_level == 0);
        }
        gpio_intr_enable(gpio_num);
        return 1;
    }

    gpio_intr_enable(gpio_num);
    return 0;
}

/* 处理 SOS 中断按键事件：消抖后判断这次变化究竟是按下还是松开。 */
int KEY_EXTI_GetEvent(int *is_pressed)
{
    return get_interrupt_key_event(KEY_EXTI_GPIO,
                                   &s_exti_irq_pending,
                                   &s_exti_stable_level,
                                   is_pressed);
}

int KEY_RECORD_IsPressed(void)
{
    return gpio_get_level(KEY_RECORD_GPIO) == 0;
}

/* 处理录音中断按键事件：消抖后判断这次变化究竟是按下还是松开。 */
int KEY_RECORD_GetEvent(int *is_pressed)
{
    return get_interrupt_key_event(KEY_RECORD_GPIO,
                                   &s_record_irq_pending,
                                   &s_record_stable_level,
                                   is_pressed);
}
