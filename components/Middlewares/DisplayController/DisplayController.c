/**
 * @file DisplayController.c
 * @brief 显示控制器——用 LVGL 库在 ST7735 TFT 上渲染系统界面。
 *
 * 使用 DMA 双缓冲：一个缓冲区 DMA 传输到 TFT 时，LVGL 在另一个缓冲区绘制下一帧。
 * flush 回调链：tft_flush_cb → esp_lcd_panel_draw_bitmap → SPI DMA → tft_flush_done → lv_display_flush_ready。
 */

#include "DisplayController.h"
#include "ui.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "BSP_TFT.h"
#include "WiFiManager.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"

static const char *TAG = "DisplayController";

#define LVGL_DMA_ALIGNMENT_BYTES  4
#define LVGL_TICK_PERIOD_MS       2
#define LVGL_TASK_STACK_SIZE      6144
#define LVGL_TASK_PRIORITY        4
#define LVGL_TASK_MIN_DELAY_MS    5
#define LVGL_TASK_MAX_DELAY_MS    20
#define LVGL_LOCK_TIMEOUT_MS      100

static bool s_enabled = false;
static lv_display_t *s_display = NULL;
static void *s_buf1 = NULL;   /* DMA 双缓冲第一块 */
static void *s_buf2 = NULL;   /* DMA 双缓冲第二块 */
static app_state_t s_current_state = APP_STATE_NORMAL;
static SemaphoreHandle_t s_lvgl_mutex = NULL;
static esp_timer_handle_t s_lvgl_tick_timer = NULL;
static TaskHandle_t s_lvgl_task_handle = NULL;

/* 缓存最新传感器数据，供独立显示任务在状态变化时刷新界面 */
static sensor_hub_data_t s_last_sensor_data = {0};
static bool s_has_sensor_data = false;

static bool lvgl_lock(TickType_t timeout_ticks)
{
    return s_lvgl_mutex != NULL
        && xSemaphoreTakeRecursive(s_lvgl_mutex, timeout_ticks) == pdTRUE;
}

static void lvgl_unlock(void)
{
    xSemaphoreGiveRecursive(s_lvgl_mutex);
}

/** 前向声明：lvgl_task 在 build_time_text 定义之前使用。 */
static void build_time_text(char *buffer, size_t buffer_size);

/** ESP 定时器仅推进 LVGL tick，不访问任何控件。 */
static void lvgl_tick_timer_cb(void *arg)
{
    (void)arg;
    lv_tick_inc(LVGL_TICK_PERIOD_MS);
}

/** 独立刷新任务保证主循环录音或播放期间动画仍能持续运行。
 *  同时检测 AppController 状态变化（如按键导致 SOS→NORMAL），
 *  在主循环阻塞时仍能立即更新屏幕显示。 */
static void lvgl_task(void *arg)
{
    (void)arg;
    TickType_t previous_tick = xTaskGetTickCount();

    for (;;) {
        uint32_t delay_ms = LVGL_TASK_MAX_DELAY_MS;
        if (lvgl_lock(portMAX_DELAY)) {
            const TickType_t current_tick = xTaskGetTickCount();
            uint32_t elapsed_ms = (uint32_t)((current_tick - previous_tick) * portTICK_PERIOD_MS);
            previous_tick = current_tick;
            if (elapsed_ms == 0) {
                elapsed_ms = LVGL_TASK_MIN_DELAY_MS;
            }

            /* 检测 AppController 状态是否已变化（如按键触发 SOS/确认），
             * 若变化则立即用缓存的传感器数据刷新界面，不等待主循环 2s 周期。 */
            const app_state_t live_state = AppController_GetState();
            if (live_state != s_current_state && s_has_sensor_data) {
                char time_text[8] = "--:--";
                build_time_text(time_text, sizeof(time_text));
                const bool online = WiFiManager_IsConnected();

                char sensor_text[64] = "--.- C";
                if (s_last_sensor_data.aht20_ok) {
                    snprintf(sensor_text, sizeof(sensor_text), "%.1f C", s_last_sensor_data.aht_temperature);
                }

                const app_state_t previous_state = s_current_state;
                s_current_state = live_state;
                ui_update(live_state, online, time_text, sensor_text);
                if (previous_state != APP_STATE_NORMAL && live_state == APP_STATE_NORMAL) {
                    ui_set_activity(DISPLAY_ACTIVITY_CONFIRMED);
                }
                ESP_LOGI(TAG, "display refreshed by lvgl_task: %s -> %s",
                         AppController_StateToString(previous_state),
                         AppController_StateToString(live_state));
            }

            ui_service_animations(s_current_state, elapsed_ms);
            delay_ms = lv_timer_handler();
            lvgl_unlock();
        }

        if (delay_ms < LVGL_TASK_MIN_DELAY_MS) delay_ms = LVGL_TASK_MIN_DELAY_MS;
        if (delay_ms > LVGL_TASK_MAX_DELAY_MS) delay_ms = LVGL_TASK_MAX_DELAY_MS;
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

/* ================================================================
 * LVGL 回调函数——连接 LVGL 和 TFT 驱动的桥梁
 * ================================================================ */

/**
 * @brief DMA 颜色传输完成回调。
 */
static bool tft_flush_done(esp_lcd_panel_io_handle_t panel_io,
                           esp_lcd_panel_io_event_data_t *edata,
                           void *user_ctx)
{
    (void)panel_io;
    (void)edata;
    lv_display_t *display = (lv_display_t *)user_ctx;
    lv_display_flush_ready(display);  /* 通知 LVGL：传输完成 */
    return false;
}

/**
 * @brief LVGL flush 回调，将像素数据发送到 TFT。
 *        包含字节序交换（lv_draw_sw_rgb565_swap）。
 */
static void tft_flush_cb(lv_display_t *display, const lv_area_t *area, uint8_t *px_map)
{
    esp_lcd_panel_handle_t panel = (esp_lcd_panel_handle_t)lv_display_get_user_data(display);
    if (panel == NULL) {
        lv_display_flush_ready(display);
        return;
    }

    int32_t width = area->x2 - area->x1 + 1;
    int32_t height = area->y2 - area->y1 + 1;
    lv_draw_sw_rgb565_swap(px_map, (uint32_t)(width * height));
    esp_lcd_panel_draw_bitmap(panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, px_map);
}

/** 构造时间字符串（HH:MM），未同步时显示 "--:--" */
static void build_time_text(char *buffer, size_t buffer_size)
{
    if (buffer == NULL || buffer_size < 6) {
        return;
    }

    time_t now = time(NULL);
    struct tm local_tm = {0};
#if defined(_WIN32)
    if (localtime_s(&local_tm, &now) != 0) { snprintf(buffer, buffer_size, "--:--"); return; }
#else
    if (localtime_r(&now, &local_tm) == NULL) { snprintf(buffer, buffer_size, "--:--"); return; }
#endif

    /* 年份小于 2024 说明 SNTP 还没同步成功 */
    if (local_tm.tm_year + 1900 < 2024) {
        snprintf(buffer, buffer_size, "--:--");
        return;
    }

    strftime(buffer, buffer_size, "%H:%M", &local_tm);
}

/* ================================================================
 * 公开接口
 * ================================================================ */

esp_err_t DisplayController_Init(void)
{
    /* 1. 初始化 TFT 硬件 */
    esp_err_t ret = BSP_TFT_Init(NULL);
    if (ret != ESP_OK) {
        s_enabled = false;
        ESP_LOGW(TAG, "TFT unavailable, display disabled: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 2. 创建 LVGL 全局递归锁，所有控件访问都必须持锁。 */
    s_lvgl_mutex = xSemaphoreCreateRecursiveMutex();
    if (s_lvgl_mutex == NULL) {
        s_enabled = false;
        return ESP_ERR_NO_MEM;
    }
    if (!lvgl_lock(portMAX_DELAY)) {
        s_enabled = false;
        return ESP_ERR_TIMEOUT;
    }

    /* 3. 初始化 LVGL 库 */
    lv_init();

    /* 4. 创建 LVGL display 对象 */
    s_display = lv_display_create(BSP_TFT_GetWidth(), BSP_TFT_GetHeight());
    if (s_display == NULL) {
        lvgl_unlock();
        s_enabled = false;
        return ESP_ERR_NO_MEM;
    }

    /* 5. 分配内部 SRAM 中显式对齐的 DMA 双缓冲。 */
    size_t draw_buffer_size = BSP_TFT_GetWidth() * BSP_TFT_DRAW_BUFFER_LINES * sizeof(lv_color16_t);
    s_buf1 = heap_caps_aligned_alloc(LVGL_DMA_ALIGNMENT_BYTES, draw_buffer_size,
                                     MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    s_buf2 = heap_caps_aligned_alloc(LVGL_DMA_ALIGNMENT_BYTES, draw_buffer_size,
                                     MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (s_buf1 == NULL || s_buf2 == NULL) {
        heap_caps_free(s_buf1);
        heap_caps_free(s_buf2);
        s_buf1 = NULL;
        s_buf2 = NULL;
        lvgl_unlock();
        s_enabled = false;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "LVGL DMA buffers=%u bytes each, internal free=%u bytes",
             (unsigned)draw_buffer_size,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    /* 6. 配置 LVGL display：双缓冲 + RGB565 颜色格式 + flush 回调 */
    lv_display_set_buffers(s_display, s_buf1, s_buf2, draw_buffer_size, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_color_format(s_display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(s_display, tft_flush_cb);
    lv_display_set_user_data(s_display, BSP_TFT_GetPanelHandle());
    lv_display_set_default(s_display);

    /* 7. 注册颜色传输完成回调（DMA 完成后通知 LVGL） */
    const bsp_tft_callbacks_t callbacks = {
        .on_color_trans_done = tft_flush_done,
        .user_ctx = s_display,
    };
    ret = BSP_TFT_RegisterCallbacks(&callbacks);
    if (ret != ESP_OK) {
        lvgl_unlock();
        s_enabled = false;
        return ret;
    }

    /* 8. 创建 UI 控件树 */
    ret = ui_init();
    lvgl_unlock();
    if (ret != ESP_OK) {
        s_enabled = false;
        return ret;
    }

    /* 9. 启动 2ms tick 定时器和独立 LVGL 刷新任务。 */
    const esp_timer_create_args_t tick_timer_args = {
        .callback = lvgl_tick_timer_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "lvgl_tick",
    };
    ret = esp_timer_create(&tick_timer_args, &s_lvgl_tick_timer);
    if (ret != ESP_OK) {
        s_enabled = false;
        return ret;
    }
    ret = esp_timer_start_periodic(s_lvgl_tick_timer, LVGL_TICK_PERIOD_MS * 1000U);
    if (ret != ESP_OK) {
        esp_timer_delete(s_lvgl_tick_timer);
        s_lvgl_tick_timer = NULL;
        s_enabled = false;
        return ret;
    }

    BaseType_t task_created = xTaskCreate(lvgl_task, "lvgl", LVGL_TASK_STACK_SIZE, NULL,
                                          LVGL_TASK_PRIORITY, &s_lvgl_task_handle);
    if (task_created != pdPASS) {
        esp_timer_stop(s_lvgl_tick_timer);
        esp_timer_delete(s_lvgl_tick_timer);
        s_lvgl_tick_timer = NULL;
        s_enabled = false;
        return ESP_ERR_NO_MEM;
    }

    s_enabled = true;
    return DisplayController_Update(APP_STATE_NORMAL, &(sensor_hub_data_t){0}, &(risk_result_t){0});
}

bool DisplayController_IsEnabled(void) { return s_enabled; }

esp_err_t DisplayController_Update(app_state_t app_state,
                                   const sensor_hub_data_t *sensor_data,
                                   const risk_result_t *risk_result)
{
    if (!s_enabled) return ESP_OK;
    if (sensor_data == NULL || risk_result == NULL) return ESP_ERR_INVALID_ARG;
    if (!lvgl_lock(pdMS_TO_TICKS(LVGL_LOCK_TIMEOUT_MS))) return ESP_ERR_TIMEOUT;

    /* 缓存最新传感器数据，供 lvgl_task 在状态变化时即时刷新 */
    s_last_sensor_data = *sensor_data;
    s_has_sensor_data = true;

    const app_state_t previous_state = s_current_state;
    s_current_state = app_state;
    char time_text[8] = "--:--";
    build_time_text(time_text, sizeof(time_text));

    const bool online = WiFiManager_IsConnected();

    /* 格式化并显示传感器数据 (现在只显示温度) */
    char sensor_text[64] = "--.- C";
    if (sensor_data->aht20_ok) {
        snprintf(sensor_text, sizeof(sensor_text), "%.1f C", sensor_data->aht_temperature);
    }

    esp_err_t ret = ui_update(app_state, online, time_text, sensor_text);
    if (ret == ESP_OK && previous_state != APP_STATE_NORMAL && app_state == APP_STATE_NORMAL) {
        ui_set_activity(DISPLAY_ACTIVITY_CONFIRMED);
    }
    lvgl_unlock();
    return ret;
}

/** 保留旧服务入口；独立 LVGL 任务已经接管 tick、动画和重绘。 */
esp_err_t DisplayController_Service(uint32_t elapsed_ms)
{
    /* 保留旧接口兼容主循环；tick 和刷新现由独立任务负责。 */
    (void)elapsed_ms;
    return ESP_OK;
}

/**
 * @brief 在屏幕上显示一条临时消息（Agent命令）
 * @param message 要显示的消息内容
 * @param duration_seconds 显示持续时间（秒）
 */
esp_err_t DisplayController_ShowMessage(const char *message, int duration_seconds)
{
    if (!s_enabled) return ESP_OK;
    if (message == NULL) return ESP_ERR_INVALID_ARG;
    if (!lvgl_lock(pdMS_TO_TICKS(LVGL_LOCK_TIMEOUT_MS))) return ESP_ERR_TIMEOUT;

    ESP_LOGI(TAG, "ShowMessage: \"%s\" for %d seconds", message, duration_seconds);

    /* 调用 UI 层显示消息，所有 LVGL 操作均处于递归锁保护内。 */
    ui_show_temporary_message(message, duration_seconds);
    lvgl_unlock();
    return ESP_OK;
}

esp_err_t DisplayController_SetActivity(display_activity_t activity)
{
    if (!s_enabled) return ESP_OK;
    if (activity < DISPLAY_ACTIVITY_NONE || activity > DISPLAY_ACTIVITY_CONFIRMED) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!lvgl_lock(pdMS_TO_TICKS(LVGL_LOCK_TIMEOUT_MS))) return ESP_ERR_TIMEOUT;

    ui_set_activity(activity);
    lvgl_unlock();
    return ESP_OK;
}
