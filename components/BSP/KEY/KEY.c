/**
 * @file KEY.c
 * @brief 按键驱动实现——包含确认键和 SOS 键的完整检测机制。
 */

#include "KEY.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"

/* ---- 引脚定义 ---- */
#define KEY_GPIO          GPIO_NUM_7   /* 确认键 */
#define KEY_EXTI_GPIO     GPIO_NUM_8   /* SOS 键 */
#define KEY_DEBOUNCE_MS   20           /* 消抖时间 (ms) */

/* ---- 确认键（轮询）状态变量 ---- */
static int s_key_stable_level = 1;       /* 消抖后的稳定电平 */
static int s_key_last_read_level = 1;    /* 上一次读取的原始电平，用于检测变化 */

/* ---- SOS 键（中断）状态变量 ---- */
static int s_exti_stable_level = 1;                    /* 消抖后的稳定电平 */
static volatile int s_exti_irq_pending = 0;            /* 中断挂起标志（volatile: ISR 和主循环共享访问） */

static int s_isr_service_installed = 0;  /* GPIO ISR 服务是否已安装 */

/**
 * @brief GPIO 中断回调，设置 pending 标志。
 * @param arg 指向对应的 irq_pending 标志变量
 */
static void IRAM_ATTR key_irq_handler(void *arg)
{
    volatile int *pending = (volatile int *)arg;
    if (pending != NULL) {
        *pending = 1;
    }
}

/* ================================================================
 * 确认键：轮询方式
 * ================================================================ */

void KEY_Init(void)
{
    /* 配置 GPIO7 为输入模式，启用内部上拉 */
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << KEY_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,      /* 上拉：默认高电平 */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,         /* 轮询不需要中断 */
    };
    gpio_config(&io_conf);

    /* 记录初始电平 */
    s_key_stable_level = gpio_get_level(KEY_GPIO);
    s_key_last_read_level = s_key_stable_level;
}

int KEY_IsPressed(void)
{
    /* 低电平有效：返回 1 表示按下 */
    return gpio_get_level(KEY_GPIO) == 0;
}

int KEY_Scan(int *is_pressed)
{
    int current_level = gpio_get_level(KEY_GPIO);

    /* 1. 电平未变则返回 */
    if (current_level == s_key_last_read_level) {
        return 0;
    }

    /* 2. 消抖等待后重新读取 */
    vTaskDelay(pdMS_TO_TICKS(KEY_DEBOUNCE_MS));
    current_level = gpio_get_level(KEY_GPIO);

    /* 3. 消抖后电平确实变化则为有效事件 */
    if (current_level != s_key_last_read_level) {
        s_key_last_read_level = current_level;

        /* 4. 稳定电平变化才报告事件 */
        if (current_level != s_key_stable_level) {
            s_key_stable_level = current_level;
            if (is_pressed != NULL) {
                *is_pressed = (s_key_stable_level == 0);  /* 0 = 低电平 = 按下 */
            }
            return 1;  /* 发生了有效按键事件 */
        }
    }

    return 0;
}

/* ================================================================
 * SOS 键：中断方式
 * ================================================================ */

void KEY_EXTI_Init(void)
{
    /* 配置 GPIO8(SOS)，双沿中断（上升沿+下降沿都触发） */
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << KEY_EXTI_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,  /* 双边沿触发：按下和松开都会进 ISR */
    };
    gpio_config(&io_conf);

    /* 记录初始电平 */
    s_exti_stable_level = gpio_get_level(KEY_EXTI_GPIO);
    s_exti_irq_pending = 0;

    /* 安装 GPIO 中断服务（全局一次） */
    if (!s_isr_service_installed) {
        gpio_install_isr_service(0);
        s_isr_service_installed = 1;
    }

    /* 绑定 SOS 引脚到 ISR 回调 */
    gpio_isr_handler_add(KEY_EXTI_GPIO, key_irq_handler, (void *)&s_exti_irq_pending);
}

int KEY_EXTI_IsPressed(void)
{
    return gpio_get_level(KEY_EXTI_GPIO) == 0;
}

/**
 * @brief 中断按键事件处理：消抖 + 判断按下/松开。
 */
static int get_interrupt_key_event(gpio_num_t gpio_num,
                                   volatile int *irq_pending,
                                   int *stable_level,
                                   int *is_pressed)
{
    /* 无待处理中断事件 */
    if (irq_pending == NULL || stable_level == NULL || !(*irq_pending)) {
        return 0;
    }

    /* 关中断 → 消抖 → 确认电平变化 */
    gpio_intr_disable(gpio_num);
    *irq_pending = 0;

    vTaskDelay(pdMS_TO_TICKS(KEY_DEBOUNCE_MS));
    int current_level = gpio_get_level(gpio_num);

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

int KEY_EXTI_GetEvent(int *is_pressed)
{
    return get_interrupt_key_event(KEY_EXTI_GPIO,
                                   &s_exti_irq_pending,
                                   &s_exti_stable_level,
                                   is_pressed);
}
