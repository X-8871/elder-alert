/**
 * @file KEY.c
 * @brief 按键驱动实现——包含确认键、SOS 键和录音键的完整检测机制。
 *
 * 【学弟必读：三个按键的接线和检测方式】
 * ┌─────────┬──────────┬──────────┬──────────────┐
 * │  按键   │  GPIO    │ 检测方式  │  功能        │
 * ├─────────┼──────────┼──────────┼──────────────┤
 * │ 确认键  │ GPIO7    │ 轮询+消抖 │ 确认提醒/解除告警 │
 * │ SOS键   │ GPIO8    │ 中断+消抖 │ 紧急求助      │
 * │ 录音键  │ GPIO17   │ 中断+消抖 │ 触发语音上传   │
 * └─────────┴──────────┴──────────┴──────────────┘
 *
 * 所有按键均为"低电平有效"：内部上拉 → 默认高电平(1) → 按下变低电平(0)。
 * 消抖窗口统一为 20ms（KEY_DEBOUNCE_MS）。
 *
 * 【中断 vs 轮询的选择原因】
 * - SOS 键用中断：紧急情况必须立即响应，不能等主循环轮询到才处理。
 * - 录音键用中断：录音时机需要精确捕获按键瞬间。
 * - 确认键用轮询：普通确认操作不需要多快的响应，轮询更简单可靠。
 */

#include "KEY.h"
#include "esp_attr.h"         /* IRAM_ATTR 宏——把 ISR 函数放到 IRAM 中保证执行速度 */
#include "freertos/FreeRTOS.h" /* vTaskDelay / TickType_t */
#include "freertos/task.h"     /* pdMS_TO_TICKS 延时转换 */
#include "driver/gpio.h"       /* ESP-IDF GPIO 驱动 */

/* ---- 引脚定义 ---- */
#define KEY_GPIO          GPIO_NUM_7   /* 确认键 */
#define KEY_EXTI_GPIO     GPIO_NUM_8   /* SOS 键 */
#define KEY_RECORD_GPIO   GPIO_NUM_17  /* 录音键 */
#define KEY_DEBOUNCE_MS   20           /* 消抖时间 (ms) */

/* ---- 确认键（轮询）状态变量 ---- */
static int s_key_stable_level = 1;       /* 消抖后的稳定电平 */
static int s_key_last_read_level = 1;    /* 上一次读取的原始电平，用于检测变化 */

/* ---- SOS 键（中断）状态变量 ---- */
static int s_exti_stable_level = 1;                    /* 消抖后的稳定电平 */
static volatile int s_exti_irq_pending = 0;            /* 中断挂起标志（volatile 因为 ISR 和主循环都会访问） */

/* ---- 录音键（中断）状态变量 ---- */
static int s_record_stable_level = 1;
static volatile int s_record_irq_pending = 0;

static int s_isr_service_installed = 0;  /* GPIO ISR 服务是否已安装（全局只需一次） */

/* ================================================================
 * 中断服务函数 (ISR, Interrupt Service Routine)
 * ================================================================
 * IRAM_ATTR 表示函数放在 IRAM（指令 RAM）中运行，不依赖 flash 缓存，
 * 保证中断响应速度。ISR 里不能做耗时操作，只设置一个标志位，让主循环去处理。
 */

/**
 * @brief GPIO 中断回调——仅仅设置 pending 标志，不做实际处理。
 * @param arg 指向对应的 irq_pending 标志变量
 */
static void IRAM_ATTR key_irq_handler(void *arg)
{
    volatile int *pending = (volatile int *)arg;
    if (pending != NULL) {
        *pending = 1;  /* 告诉主循环：有按键事件需要处理 */
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

    /* 记录初始电平作为"稳定状态"的起点 */
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

    /* 1. 快速检查：电平没变就直接返回 */
    if (current_level == s_key_last_read_level) {
        return 0;
    }

    /* 2. 电平变了，等 20ms 消抖后再读一次 */
    vTaskDelay(pdMS_TO_TICKS(KEY_DEBOUNCE_MS));
    current_level = gpio_get_level(KEY_GPIO);

    /* 3. 如果消抖后电平和之前记录的不一致，说明这次变化是真实的 */
    if (current_level != s_key_last_read_level) {
        s_key_last_read_level = current_level;

        /* 4. 只有稳定电平确实改变了才报告事件 */
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
 * SOS 键 + 录音键：中断方式
 * ================================================================ */

void KEY_EXTI_Init(void)
{
    /* 同时配置 GPIO8(SOS) 和 GPIO17(录音)，双沿中断（上升沿+下降沿都触发） */
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << KEY_EXTI_GPIO) | (1ULL << KEY_RECORD_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,  /* 双边沿触发：按下和松开都会进 ISR */
    };
    gpio_config(&io_conf);

    /* 记录初始电平 */
    s_exti_stable_level = gpio_get_level(KEY_EXTI_GPIO);
    s_exti_irq_pending = 0;
    s_record_stable_level = gpio_get_level(KEY_RECORD_GPIO);
    s_record_irq_pending = 0;

    /* GPIO 中断服务全局只需安装一次（不是每个引脚安装一次） */
    if (!s_isr_service_installed) {
        gpio_install_isr_service(0);  /* 参数 0 表示不指定标志位 */
        s_isr_service_installed = 1;
    }

    /* 分别绑定两个引脚到同一个 ISR 回调，但传入不同的 pending 标志 */
    gpio_isr_handler_add(KEY_EXTI_GPIO, key_irq_handler, (void *)&s_exti_irq_pending);
    gpio_isr_handler_add(KEY_RECORD_GPIO, key_irq_handler, (void *)&s_record_irq_pending);
}

int KEY_EXTI_IsPressed(void)
{
    return gpio_get_level(KEY_EXTI_GPIO) == 0;
}

/**
 * @brief 通用的中断按键事件处理函数——消抖 + 判断按下/松开。
 *
 * 流程：
 * 1. 检查是否有待处理的中断事件（irq_pending 标志）
 * 2. 临时关闭该 GPIO 的中断（防止消抖期间再次触发）
 * 3. 等待 20ms 消抖
 * 4. 读取电平，如果确实变了就报告事件
 * 5. 重新打开中断
 */
static int get_interrupt_key_event(gpio_num_t gpio_num,
                                   volatile int *irq_pending,
                                   int *stable_level,
                                   int *is_pressed)
{
    /* 没有待处理的中断事件 */
    if (irq_pending == NULL || stable_level == NULL || !(*irq_pending)) {
        return 0;
    }

    /* 关闭中断 → 消抖等待 → 确认电平变化 */
    gpio_intr_disable(gpio_num);
    *irq_pending = 0;  /* 清除 pending 标志 */

    vTaskDelay(pdMS_TO_TICKS(KEY_DEBOUNCE_MS));
    int current_level = gpio_get_level(gpio_num);

    if (current_level != *stable_level) {
        *stable_level = current_level;
        if (is_pressed != NULL) {
            *is_pressed = (*stable_level == 0);
        }
        gpio_intr_enable(gpio_num);  /* 恢复中断 */
        return 1;
    }

    gpio_intr_enable(gpio_num);  /* 恢复中断（虽然是抖动也恢复） */
    return 0;
}

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

int KEY_RECORD_GetEvent(int *is_pressed)
{
    return get_interrupt_key_event(KEY_RECORD_GPIO,
                                   &s_record_irq_pending,
                                   &s_record_stable_level,
                                   is_pressed);
}
