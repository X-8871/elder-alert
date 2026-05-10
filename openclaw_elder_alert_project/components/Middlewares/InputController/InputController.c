/**
 * @file InputController.c
 * @brief 输入控制器实现，封装确认键短按/长按、SOS 键和录音键的事件检测逻辑。
 *
 * 确认键（GPIO7）：轮询消抖，支持短按事件和 8 秒长按事件（用于配网重置）。
 * SOS 键（GPIO8）：中断驱动，一次性事件，触发后需重新等待下次中断。
 * 录音键（GPIO17）：中断驱动，一次性事件，触发后需重新等待下次中断。
 */

#include "InputController.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "KEY.h"

#define CONFIRM_LONG_PRESS_MS 8000U

static const char *TAG = "InputController";
static bool s_initialized = false;
static bool s_confirm_pressed = false;
static bool s_confirm_long_reported = false;
static bool s_confirm_short_pending = false;
static bool s_confirm_long_pending = false;
static TickType_t s_confirm_press_tick = 0;

static void update_confirm_key_state(void)
{
    int is_pressed = 0;
    TickType_t now = xTaskGetTickCount();

    if (KEY_Scan(&is_pressed)) {
        if (is_pressed) {
            s_confirm_pressed = true;
            s_confirm_long_reported = false;
            s_confirm_press_tick = now;
        } else {
            if (s_confirm_pressed && !s_confirm_long_reported) {
                s_confirm_short_pending = true;
            }
            s_confirm_pressed = false;
            s_confirm_long_reported = false;
        }
    }

    if (s_confirm_pressed && !s_confirm_long_reported) {
        uint32_t held_ms = (uint32_t)((now - s_confirm_press_tick) * portTICK_PERIOD_MS);
        if (held_ms >= CONFIRM_LONG_PRESS_MS) {
            s_confirm_long_reported = true;
            s_confirm_long_pending = true;
            s_confirm_short_pending = false;
            ESP_LOGW(TAG, "confirm key long press detected, held_ms=%lu", (unsigned long)held_ms);
        }
    }
}

esp_err_t InputController_Init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    /* GPIO7 走轮询消抖，GPIO8/GPIO17 走中断事件，由底层 KEY 模块分别初始化。 */
    KEY_Init();
    KEY_EXTI_Init();

    s_confirm_pressed = KEY_IsPressed();
    s_confirm_long_reported = false;
    s_confirm_short_pending = false;
    s_confirm_long_pending = false;
    s_confirm_press_tick = xTaskGetTickCount();

    s_initialized = true;
    ESP_LOGI(TAG, "confirm key ready on GPIO7");
    ESP_LOGI(TAG, "sos key ready on GPIO8");
    ESP_LOGI(TAG, "record key ready on GPIO17");
    return ESP_OK;
}

esp_err_t InputController_GetConfirmEvent(bool *confirmed)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (confirmed == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *confirmed = false;

    update_confirm_key_state();
    if (s_confirm_short_pending) {
        s_confirm_short_pending = false;
        *confirmed = true;
    }

    return ESP_OK;
}

esp_err_t InputController_GetRecordEvent(bool *record_triggered)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (record_triggered == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    int is_pressed = 0;
    *record_triggered = false;

    if (KEY_RECORD_GetEvent(&is_pressed) && is_pressed) {
        *record_triggered = true;
    }

    return ESP_OK;
}

esp_err_t InputController_GetConfirmLongPressEvent(bool *long_pressed)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (long_pressed == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *long_pressed = false;
    update_confirm_key_state();
    if (s_confirm_long_pending) {
        s_confirm_long_pending = false;
        *long_pressed = true;
    }

    return ESP_OK;
}

esp_err_t InputController_GetSosEvent(bool *sos_triggered)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (sos_triggered == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    int is_pressed = 0;
    *sos_triggered = false;

    /* SOS 键走独立中断输入脚，底层把它包装成一次性事件。 */
    if (KEY_EXTI_GetEvent(&is_pressed) && is_pressed) {
        *sos_triggered = true;
    }

    return ESP_OK;
}
