#include "DisplayController.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "BSP_TFT.h"
#include "WiFiManager.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"

LV_FONT_DECLARE(lv_font_source_han_sans_sc_16_cjk);

static const char *TAG = "DisplayController";

static bool s_enabled = false;
static lv_display_t *s_display = NULL;
static void *s_buf1 = NULL;
static void *s_buf2 = NULL;

static lv_obj_t *s_screen = NULL;
static lv_obj_t *s_top_bar = NULL;
static lv_obj_t *s_time_label = NULL;
static lv_obj_t *s_network_label = NULL;
static lv_obj_t *s_offline_banner = NULL;
static lv_obj_t *s_state_label = NULL;
static lv_obj_t *s_reason_label = NULL;
static lv_obj_t *s_prompt_label = NULL;
static lv_obj_t *s_confirm_button = NULL;
static lv_obj_t *s_sos_button = NULL;
static lv_obj_t *s_confirm_label = NULL;
static lv_obj_t *s_sos_label = NULL;

static bool tft_flush_done(esp_lcd_panel_io_handle_t panel_io,
                           esp_lcd_panel_io_event_data_t *edata,
                           void *user_ctx)
{
    (void)panel_io;
    (void)edata;
    lv_display_t *display = (lv_display_t *)user_ctx;
    lv_display_flush_ready(display);
    return false;
}

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

static const lv_font_t *ui_font(void)
{
    return &lv_font_source_han_sans_sc_16_cjk;
}

static lv_color_t state_bg_color(app_state_t state)
{
    switch (state) {
    case APP_STATE_NORMAL:
        return lv_color_hex(0x1E9E55);
    case APP_STATE_REMIND:
        return lv_color_hex(0xE58A1F);
    case APP_STATE_ALARM:
        return lv_color_hex(0xD64633);
    case APP_STATE_SOS:
        return lv_color_hex(0xA61E22);
    default:
        return lv_color_hex(0x5E5E5E);
    }
}

static const char *state_text(app_state_t state)
{
    switch (state) {
    case APP_STATE_NORMAL:
        return "当前正常";
    case APP_STATE_REMIND:
        return "请确认";
    case APP_STATE_ALARM:
        return "需要帮助";
    case APP_STATE_SOS:
        return "已求助";
    default:
        return "状态未知";
    }
}

static const char *state_prompt_text(app_state_t state)
{
    switch (state) {
    case APP_STATE_NORMAL:
        return "设备正在守护您";
    case APP_STATE_REMIND:
        return "请按确认键";
    case APP_STATE_ALARM:
        return "请尽快确认";
    case APP_STATE_SOS:
        return "确认后可解除";
    default:
        return "";
    }
}

static const char *build_short_reason(app_state_t app_state,
                                      const sensor_hub_data_t *sensor_data,
                                      const risk_result_t *risk_result)
{
    (void)sensor_data;

    if (app_state == APP_STATE_NORMAL) {
        return WiFiManager_IsConnected() ? "本地提醒已就绪" : "离线时本地提醒仍有效";
    }
    if (app_state == APP_STATE_REMIND) {
        if (risk_result->static_presence_no_motion || risk_result->no_motion_timeout) {
            return "长时间无活动";
        }
        return "请确认当前情况";
    }
    if (app_state == APP_STATE_ALARM) {
        if (risk_result->mq2_warning) {
            return "烟雾异常";
        }
        if (risk_result->high_temperature || risk_result->high_humidity) {
            return "环境异常";
        }
        if (risk_result->low_light_no_motion || risk_result->static_presence_no_motion || risk_result->no_motion_timeout) {
            return "活动异常";
        }
        return "检测到异常情况";
    }
    if (app_state == APP_STATE_SOS) {
        return "等待家属联系";
    }
    return "";
}

static void build_time_text(char *buffer, size_t buffer_size)
{
    if (buffer == NULL || buffer_size < 6) {
        return;
    }

    time_t now = time(NULL);
    struct tm local_tm = {0};
#if defined(_WIN32)
    if (localtime_s(&local_tm, &now) != 0) {
        snprintf(buffer, buffer_size, "--:--");
        return;
    }
#else
    if (localtime_r(&now, &local_tm) == NULL) {
        snprintf(buffer, buffer_size, "--:--");
        return;
    }
#endif

    if (local_tm.tm_year + 1900 < 2024) {
        snprintf(buffer, buffer_size, "--:--");
        return;
    }

    strftime(buffer, buffer_size, "%H:%M", &local_tm);
}

static void style_button_box(lv_obj_t *obj, lv_color_t bg_color)
{
    lv_obj_set_style_radius(obj, 8, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(obj, bg_color, 0);
    lv_obj_set_style_pad_all(obj, 4, 0);
}

static esp_err_t build_ui_tree(void)
{
    s_screen = lv_obj_create(NULL);
    if (s_screen == NULL) {
        return ESP_ERR_NO_MEM;
    }

    lv_obj_remove_style_all(s_screen);
    lv_obj_set_size(s_screen, BSP_TFT_GetWidth(), BSP_TFT_GetHeight());
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(0x1E9E55), 0);
    lv_obj_set_style_pad_all(s_screen, 0, 0);

    s_top_bar = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_top_bar);
    lv_obj_set_size(s_top_bar, BSP_TFT_GetWidth(), 20);
    lv_obj_align(s_top_bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_opa(s_top_bar, LV_OPA_90, 0);
    lv_obj_set_style_bg_color(s_top_bar, lv_color_white(), 0);
    lv_obj_set_style_pad_hor(s_top_bar, 6, 0);
    lv_obj_set_style_pad_ver(s_top_bar, 2, 0);

    s_time_label = lv_label_create(s_top_bar);
    lv_obj_set_style_text_font(s_time_label, ui_font(), 0);
    lv_obj_set_style_text_color(s_time_label, lv_color_hex(0x1E1E1E), 0);
    lv_obj_align(s_time_label, LV_ALIGN_LEFT_MID, 0, 0);

    s_network_label = lv_label_create(s_top_bar);
    lv_obj_set_style_text_font(s_network_label, ui_font(), 0);
    lv_obj_set_style_text_color(s_network_label, lv_color_hex(0x1E1E1E), 0);
    lv_obj_align(s_network_label, LV_ALIGN_RIGHT_MID, 0, 0);

    s_offline_banner = lv_label_create(s_screen);
    lv_obj_set_style_text_font(s_offline_banner, ui_font(), 0);
    lv_obj_set_style_text_color(s_offline_banner, lv_color_white(), 0);
    lv_label_set_text(s_offline_banner, "离线模式");
    lv_obj_align(s_offline_banner, LV_ALIGN_TOP_MID, 0, 24);

    s_state_label = lv_label_create(s_screen);
    lv_obj_set_width(s_state_label, 112);
    lv_label_set_long_mode(s_state_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_state_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_state_label, ui_font(), 0);
    lv_obj_set_style_text_color(s_state_label, lv_color_white(), 0);
    lv_obj_align(s_state_label, LV_ALIGN_TOP_MID, 0, 46);

    s_reason_label = lv_label_create(s_screen);
    lv_obj_set_width(s_reason_label, 114);
    lv_label_set_long_mode(s_reason_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_reason_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_reason_label, ui_font(), 0);
    lv_obj_set_style_text_color(s_reason_label, lv_color_white(), 0);
    lv_obj_align(s_reason_label, LV_ALIGN_TOP_MID, 0, 74);

    s_prompt_label = lv_label_create(s_screen);
    lv_obj_set_width(s_prompt_label, 114);
    lv_label_set_long_mode(s_prompt_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_prompt_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_prompt_label, ui_font(), 0);
    lv_obj_set_style_text_color(s_prompt_label, lv_color_hex(0xF7F7F7), 0);
    lv_obj_align(s_prompt_label, LV_ALIGN_TOP_MID, 0, 102);

    s_confirm_button = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_confirm_button);
    lv_obj_set_size(s_confirm_button, 56, 28);
    lv_obj_align(s_confirm_button, LV_ALIGN_BOTTOM_LEFT, 6, -8);
    style_button_box(s_confirm_button, lv_color_hex(0x147A46));

    s_confirm_label = lv_label_create(s_confirm_button);
    lv_obj_set_style_text_font(s_confirm_label, ui_font(), 0);
    lv_obj_set_style_text_color(s_confirm_label, lv_color_white(), 0);
    lv_label_set_text(s_confirm_label, "确认");
    lv_obj_center(s_confirm_label);

    s_sos_button = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_sos_button);
    lv_obj_set_size(s_sos_button, 56, 28);
    lv_obj_align(s_sos_button, LV_ALIGN_BOTTOM_RIGHT, -6, -8);
    style_button_box(s_sos_button, lv_color_hex(0xC92F2A));

    s_sos_label = lv_label_create(s_sos_button);
    lv_obj_set_style_text_font(s_sos_label, ui_font(), 0);
    lv_obj_set_style_text_color(s_sos_label, lv_color_white(), 0);
    lv_label_set_text(s_sos_label, "SOS");
    lv_obj_center(s_sos_label);

    lv_screen_load(s_screen);
    return ESP_OK;
}

esp_err_t DisplayController_Init(void)
{
    esp_err_t ret = BSP_TFT_Init(NULL);
    if (ret != ESP_OK) {
        s_enabled = false;
        ESP_LOGW(TAG, "TFT unavailable, display disabled: %s", esp_err_to_name(ret));
        return ret;
    }

    lv_init();

    s_display = lv_display_create(BSP_TFT_GetWidth(), BSP_TFT_GetHeight());
    if (s_display == NULL) {
        s_enabled = false;
        return ESP_ERR_NO_MEM;
    }

    size_t draw_buffer_size = BSP_TFT_GetWidth() * BSP_TFT_DRAW_BUFFER_LINES * sizeof(lv_color16_t);
    s_buf1 = heap_caps_malloc(draw_buffer_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    s_buf2 = heap_caps_malloc(draw_buffer_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (s_buf1 == NULL || s_buf2 == NULL) {
        s_enabled = false;
        return ESP_ERR_NO_MEM;
    }

    lv_display_set_buffers(s_display, s_buf1, s_buf2, draw_buffer_size, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_color_format(s_display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(s_display, tft_flush_cb);
    lv_display_set_user_data(s_display, BSP_TFT_GetPanelHandle());
    lv_display_set_default(s_display);

    const bsp_tft_callbacks_t callbacks = {
        .on_color_trans_done = tft_flush_done,
        .user_ctx = s_display,
    };
    ret = BSP_TFT_RegisterCallbacks(&callbacks);
    if (ret != ESP_OK) {
        s_enabled = false;
        return ret;
    }

    ret = build_ui_tree();
    if (ret != ESP_OK) {
        s_enabled = false;
        return ret;
    }

    s_enabled = true;
    return DisplayController_Update(APP_STATE_NORMAL, &(sensor_hub_data_t){0}, &(risk_result_t){0});
}

bool DisplayController_IsEnabled(void)
{
    return s_enabled;
}

esp_err_t DisplayController_Update(app_state_t app_state,
                                   const sensor_hub_data_t *sensor_data,
                                   const risk_result_t *risk_result)
{
    if (!s_enabled) {
        return ESP_OK;
    }
    if (sensor_data == NULL || risk_result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char time_text[8] = "--:--";
    build_time_text(time_text, sizeof(time_text));

    bool online = WiFiManager_IsConnected();
    lv_color_t bg = state_bg_color(app_state);

    lv_obj_set_style_bg_color(s_screen, bg, 0);
    lv_obj_set_style_bg_color(s_top_bar, online ? lv_color_white() : lv_color_hex(0x676767), 0);
    lv_obj_set_style_text_color(s_time_label, online ? lv_color_hex(0x1E1E1E) : lv_color_white(), 0);
    lv_obj_set_style_text_color(s_network_label, online ? lv_color_hex(0x1E1E1E) : lv_color_white(), 0);

    lv_label_set_text(s_time_label, time_text);
    lv_label_set_text(s_network_label, online ? "已联网" : "离线");
    if (online) {
        lv_obj_add_flag(s_offline_banner, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(s_offline_banner, LV_OBJ_FLAG_HIDDEN);
    }
    lv_label_set_text(s_state_label, state_text(app_state));
    lv_label_set_text(s_reason_label, build_short_reason(app_state, sensor_data, risk_result));
    lv_label_set_text(s_prompt_label, state_prompt_text(app_state));

    lv_timer_handler();
    return ESP_OK;
}

esp_err_t DisplayController_Service(uint32_t elapsed_ms)
{
    if (!s_enabled) {
        return ESP_OK;
    }

    lv_tick_inc(elapsed_ms);
    lv_timer_handler();
    return ESP_OK;
}
