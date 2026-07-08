#include "ui.h"
#include "lvgl.h"
#include "BSP_TFT.h"
#include <stdio.h>

/* ---- UI 布局常量（像素）---- */
#define UI_TOP_BAR_HEIGHT           20
#define UI_BOTTOM_PANEL_HEIGHT      28
#define UI_ACTIVITY_BAR_COUNT       4
#define UI_ACTIVITY_FRAME_MS        100
#define UI_CONFIRMED_DURATION_MS    1200

/* ---- LVGL 控件句柄 ---- */
static lv_obj_t *s_screen = NULL;
static lv_obj_t *s_top_bar = NULL;
static lv_obj_t *s_time_label = NULL;
static lv_obj_t *s_network_label = NULL;

static lv_obj_t *s_status_label = NULL;
static lv_obj_t *s_temp_label = NULL;

static lv_obj_t *s_bottom_panel = NULL;
static lv_obj_t *s_confirm_button = NULL;
static lv_obj_t *s_sos_button = NULL;
static lv_obj_t *s_activity_panel = NULL;
static lv_obj_t *s_activity_label = NULL;
static lv_obj_t *s_activity_bars[UI_ACTIVITY_BAR_COUNT] = {0};
static lv_obj_t *s_activity_check = NULL;
static bool s_online = false;
static bool s_alert_active = false;
static display_activity_t s_activity = DISPLAY_ACTIVITY_NONE;
static uint32_t s_activity_elapsed_ms = 0;
static uint32_t s_activity_frame_elapsed_ms = 0;

static const lv_point_precise_t s_check_points[] = {
    {0, 10},
    {8, 18},
    {25, 0},
};

/* 声明字体 */
LV_FONT_DECLARE(lv_font_montserrat_14);
LV_FONT_DECLARE(lv_font_montserrat_20);
LV_FONT_DECLARE(lv_font_montserrat_32);

static const lv_font_t *font_small = &lv_font_montserrat_14;
static const lv_font_t *font_mid   = &lv_font_montserrat_20;
static const lv_font_t *font_large = &lv_font_montserrat_32;

static esp_err_t create_top_bar(void)
{
    s_top_bar = lv_obj_create(s_screen);
    if (s_top_bar == NULL) return ESP_ERR_NO_MEM;

    lv_obj_remove_style_all(s_top_bar);
    lv_obj_set_size(s_top_bar, BSP_TFT_GetWidth(), UI_TOP_BAR_HEIGHT);
    lv_obj_align(s_top_bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_opa(s_top_bar, LV_OPA_60, 0);
    lv_obj_set_style_bg_color(s_top_bar, lv_color_black(), 0);
    lv_obj_set_style_pad_hor(s_top_bar, 5, 0);

    s_time_label = lv_label_create(s_top_bar);
    lv_obj_set_style_text_font(s_time_label, font_small, 0);
    lv_obj_set_style_text_color(s_time_label, lv_color_white(), 0);
    lv_obj_align(s_time_label, LV_ALIGN_LEFT_MID, 0, 0);

    s_network_label = lv_label_create(s_top_bar);
    lv_obj_set_style_text_font(s_network_label, font_small, 0);
    lv_obj_set_style_text_color(s_network_label, lv_color_white(), 0);
    lv_obj_align(s_network_label, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_label_set_text(s_network_label, LV_SYMBOL_WIFI);

    return ESP_OK;
}

static esp_err_t create_center_area(void)
{
    /* 状态指示（如 NORMAL, SOS） */
    s_status_label = lv_label_create(s_screen);
    lv_obj_set_style_text_font(s_status_label, font_mid, 0);
    lv_obj_set_style_text_color(s_status_label, lv_color_white(), 0);
    lv_obj_align(s_status_label, LV_ALIGN_TOP_MID, 0, 26);
    lv_label_set_text(s_status_label, "NORMAL");

    /* 温度大字显示 */
    s_temp_label = lv_label_create(s_screen);
    if (s_temp_label == NULL) return ESP_ERR_NO_MEM;
    lv_obj_set_style_text_font(s_temp_label, font_large, 0);
    lv_obj_set_style_text_color(s_temp_label, lv_color_white(), 0);
    lv_obj_align(s_temp_label, LV_ALIGN_CENTER, 0, 8);
    lv_label_set_text(s_temp_label, "--.- C");

    return ESP_OK;
}

static void style_btn(lv_obj_t *obj, lv_color_t color)
{
    lv_obj_set_style_radius(obj, 6, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(obj, color, 0);
    lv_obj_set_style_shadow_width(obj, 0, 0);
}

static esp_err_t create_button_row(void)
{
    s_bottom_panel = lv_obj_create(s_screen);
    if (s_bottom_panel == NULL) return ESP_ERR_NO_MEM;
    lv_obj_remove_style_all(s_bottom_panel);
    lv_obj_set_size(s_bottom_panel, BSP_TFT_GetWidth(), UI_BOTTOM_PANEL_HEIGHT);
    lv_obj_align(s_bottom_panel, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(s_bottom_panel, LV_OPA_60, 0);
    lv_obj_set_style_bg_color(s_bottom_panel, lv_color_black(), 0);

    /* SOS 按钮（左侧） */
    s_sos_button = lv_obj_create(s_bottom_panel);
    lv_obj_remove_style_all(s_sos_button);
    lv_obj_set_size(s_sos_button, 50, 20);
    lv_obj_align(s_sos_button, LV_ALIGN_LEFT_MID, 4, 0);
    style_btn(s_sos_button, lv_color_hex(0xC0392B));

    lv_obj_t *l1 = lv_label_create(s_sos_button);
    lv_obj_set_style_text_font(l1, font_small, 0);
    lv_obj_set_style_text_color(l1, lv_color_white(), 0);
    lv_label_set_text(l1, "SOS");
    lv_obj_center(l1);

    /* OK 按钮（右侧） */
    s_confirm_button = lv_obj_create(s_bottom_panel);
    lv_obj_remove_style_all(s_confirm_button);
    lv_obj_set_size(s_confirm_button, 50, 20);
    lv_obj_align(s_confirm_button, LV_ALIGN_RIGHT_MID, -4, 0);
    style_btn(s_confirm_button, lv_color_hex(0x27AE60));

    lv_obj_t *l2 = lv_label_create(s_confirm_button);
    lv_obj_set_style_text_font(l2, font_small, 0);
    lv_obj_set_style_text_color(l2, lv_color_white(), 0);
    lv_label_set_text(l2, "OK");
    lv_obj_center(l2);

    return ESP_OK;
}

static esp_err_t create_activity_overlay(void)
{
    s_activity_panel = lv_obj_create(s_screen);
    if (s_activity_panel == NULL) return ESP_ERR_NO_MEM;
    lv_obj_remove_style_all(s_activity_panel);
    lv_obj_set_size(s_activity_panel, 144, 72);
    lv_obj_align(s_activity_panel, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(s_activity_panel, 12, 0);
    lv_obj_set_style_bg_opa(s_activity_panel, LV_OPA_90, 0);
    lv_obj_set_style_bg_color(s_activity_panel, lv_color_hex(0x172A3A), 0);
    lv_obj_set_style_border_width(s_activity_panel, 1, 0);
    lv_obj_set_style_border_color(s_activity_panel, lv_color_hex(0x6EC5E9), 0);

    s_activity_label = lv_label_create(s_activity_panel);
    if (s_activity_label == NULL) return ESP_ERR_NO_MEM;
    lv_obj_set_style_text_font(s_activity_label, font_small, 0);
    lv_obj_set_style_text_color(s_activity_label, lv_color_white(), 0);
    lv_obj_align(s_activity_label, LV_ALIGN_TOP_MID, 0, 7);

    for (int i = 0; i < UI_ACTIVITY_BAR_COUNT; ++i) {
        s_activity_bars[i] = lv_obj_create(s_activity_panel);
        if (s_activity_bars[i] == NULL) return ESP_ERR_NO_MEM;
        lv_obj_remove_style_all(s_activity_bars[i]);
        lv_obj_set_size(s_activity_bars[i], 8, 12);
        lv_obj_set_style_radius(s_activity_bars[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(s_activity_bars[i], LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(s_activity_bars[i], lv_color_hex(0x6EC5E9), 0);
        lv_obj_align(s_activity_bars[i], LV_ALIGN_BOTTOM_MID, -24 + i * 16, -10);
    }

    s_activity_check = lv_line_create(s_activity_panel);
    if (s_activity_check == NULL) return ESP_ERR_NO_MEM;
    lv_line_set_points(s_activity_check, s_check_points, 3);
    lv_obj_set_size(s_activity_check, 28, 22);
    lv_obj_set_style_line_width(s_activity_check, 5, 0);
    lv_obj_set_style_line_rounded(s_activity_check, true, 0);
    lv_obj_set_style_line_color(s_activity_check, lv_color_hex(0x9FE3B1), 0);
    lv_obj_align(s_activity_check, LV_ALIGN_CENTER, 0, 9);
    lv_obj_add_flag(s_activity_check, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_activity_panel, LV_OBJ_FLAG_HIDDEN);
    return ESP_OK;
}

static bool activity_can_show(void)
{
    return s_activity != DISPLAY_ACTIVITY_NONE && s_online && !s_alert_active;
}

static void refresh_activity_visibility(void)
{
    if (activity_can_show()) {
        lv_obj_remove_flag(s_activity_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_activity_panel);
        lv_obj_move_foreground(s_bottom_panel);
    } else {
        lv_obj_add_flag(s_activity_panel, LV_OBJ_FLAG_HIDDEN);
    }
}

static void show_activity_bars(bool visible)
{
    for (int i = 0; i < UI_ACTIVITY_BAR_COUNT; ++i) {
        if (visible) {
            lv_obj_remove_flag(s_activity_bars[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_activity_bars[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void ui_set_activity(display_activity_t activity)
{
    s_activity = activity;
    s_activity_elapsed_ms = 0;
    s_activity_frame_elapsed_ms = 0;

    if (activity == DISPLAY_ACTIVITY_NONE) {
        refresh_activity_visibility();
        return;
    }

    lv_obj_set_style_bg_color(s_activity_panel,
                              activity == DISPLAY_ACTIVITY_CONFIRMED
                                  ? lv_color_hex(0x165D3A)
                                  : lv_color_hex(0x172A3A),
                              0);
    lv_obj_set_style_border_color(s_activity_panel,
                                  activity == DISPLAY_ACTIVITY_CONFIRMED
                                      ? lv_color_hex(0x9FE3B1)
                                      : lv_color_hex(0x6EC5E9),
                                  0);

    if (activity == DISPLAY_ACTIVITY_CONFIRMED) {
        lv_label_set_text(s_activity_label, "CONFIRMED");
        show_activity_bars(false);
        lv_obj_remove_flag(s_activity_check, LV_OBJ_FLAG_HIDDEN);
    } else {
        const char *label = activity == DISPLAY_ACTIVITY_LISTENING ? "LISTENING"
                            : activity == DISPLAY_ACTIVITY_PROCESSING ? "PROCESSING"
                            : "SPEAKING";
        lv_label_set_text(s_activity_label, label);
        show_activity_bars(true);
        lv_obj_add_flag(s_activity_check, LV_OBJ_FLAG_HIDDEN);
    }
    refresh_activity_visibility();
}

esp_err_t ui_init(void)
{
    s_screen = lv_obj_create(NULL);
    if (s_screen == NULL) return ESP_ERR_NO_MEM;

    lv_obj_remove_style_all(s_screen);
    lv_obj_set_size(s_screen, BSP_TFT_GetWidth(), BSP_TFT_GetHeight());
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(0x27AE60), 0);  /* 默认绿色底色 */

    esp_err_t ret = create_top_bar();
    if (ret != ESP_OK) return ret;
    ret = create_center_area();
    if (ret != ESP_OK) return ret;
    ret = create_button_row();
    if (ret != ESP_OK) return ret;
    ret = create_activity_overlay();
    if (ret != ESP_OK) return ret;

    lv_screen_load(s_screen);
    return ESP_OK;
}

esp_err_t ui_update(app_state_t app_state, bool online, const char *time_text, const char *sensor_text)
{
    s_online = online;
    s_alert_active = app_state == APP_STATE_ALARM || app_state == APP_STATE_SOS;

    /* 1. 更新顶部状态 */
    lv_label_set_text(s_time_label, time_text);
    if (online) {
        lv_obj_set_style_text_color(s_network_label, lv_color_white(), 0);
    } else {
        lv_obj_set_style_text_color(s_network_label, lv_color_hex(0x888888), 0);
    }

    /* 2. 更新中央温度 */
    lv_label_set_text(s_temp_label, sensor_text);

    /* SOS/ALARM 优先于离线提示，确保本地紧急状态始终可见。 */
    if (app_state == APP_STATE_SOS) {
        lv_obj_set_style_bg_color(s_screen, lv_color_hex(0x8E1B16), 0);
        lv_label_set_text(s_status_label, "SOS!");
    } else if (app_state == APP_STATE_ALARM) {
        lv_obj_set_style_bg_color(s_screen, lv_color_hex(0xC0392B), 0);
        lv_label_set_text(s_status_label, "ALARM");
    } else if (app_state == APP_STATE_REMIND) {
        lv_obj_set_style_bg_color(s_screen, lv_color_hex(0xF39C12), 0); /* 橙色背景 */
        lv_label_set_text(s_status_label, "WARNING");
    } else {
        lv_obj_set_style_bg_color(s_screen, lv_color_hex(0x27AE60), 0); /* 绿色背景 */
        lv_label_set_text(s_status_label, "NORMAL");
    }
    refresh_activity_visibility();

    return ESP_OK;
}

static void update_activity_frame(void)
{
    const uint32_t phase = (s_activity_elapsed_ms / UI_ACTIVITY_FRAME_MS) % UI_ACTIVITY_BAR_COUNT;
    static const int16_t listening_heights[UI_ACTIVITY_BAR_COUNT] = {12, 26, 18, 32};
    static const int16_t speaking_heights[UI_ACTIVITY_BAR_COUNT] = {28, 14, 32, 20};

    if (s_activity == DISPLAY_ACTIVITY_PROCESSING) {
        for (int i = 0; i < UI_ACTIVITY_BAR_COUNT; ++i) {
            const bool active = (uint32_t)i == phase;
            lv_obj_set_size(s_activity_bars[i], 8, 8);
            lv_obj_set_style_bg_opa(s_activity_bars[i], active ? LV_OPA_COVER : LV_OPA_40, 0);
            lv_obj_align(s_activity_bars[i], LV_ALIGN_BOTTOM_MID, -24 + i * 16, active ? -18 : -10);
        }
        return;
    }

    const int16_t *heights = s_activity == DISPLAY_ACTIVITY_LISTENING
                                 ? listening_heights
                                 : speaking_heights;
    for (int i = 0; i < UI_ACTIVITY_BAR_COUNT; ++i) {
        const int pattern_index = (i + (int)phase) % UI_ACTIVITY_BAR_COUNT;
        const int16_t height = heights[pattern_index];
        lv_obj_set_size(s_activity_bars[i], 8, height);
        lv_obj_set_style_bg_opa(s_activity_bars[i], LV_OPA_COVER, 0);
        lv_obj_align(s_activity_bars[i], LV_ALIGN_BOTTOM_MID, -24 + i * 16, -8);
    }
}

void ui_service_animations(app_state_t app_state, uint32_t elapsed_ms)
{
    static uint32_t anim_timer = 0;

    if (s_activity != DISPLAY_ACTIVITY_NONE) {
        s_activity_elapsed_ms += elapsed_ms;
        s_activity_frame_elapsed_ms += elapsed_ms;
        if (s_activity == DISPLAY_ACTIVITY_CONFIRMED
            && s_activity_elapsed_ms >= UI_CONFIRMED_DURATION_MS) {
            ui_set_activity(DISPLAY_ACTIVITY_NONE);
        } else if (activity_can_show()
                   && s_activity != DISPLAY_ACTIVITY_CONFIRMED
                   && s_activity_frame_elapsed_ms >= UI_ACTIVITY_FRAME_MS) {
            s_activity_frame_elapsed_ms = 0;
            update_activity_frame();
        }
    }

/* 离线时仍正常显示四态状态机，仅网络图标变灰作为提示。 */

    anim_timer += elapsed_ms;

    /* 危险状态下，背景红色与暗红交替闪烁，引起注意 */
    if (app_state == APP_STATE_ALARM || app_state == APP_STATE_SOS) {
        if (anim_timer > 300) {
            anim_timer = 0;
            static bool toggle = false;
            toggle = !toggle;

            lv_color_t bg = toggle ? lv_color_hex(0xC0392B) : lv_color_hex(0x641E16);
            lv_obj_set_style_bg_color(s_screen, bg, 0);
        }
    }
}

/* ---- 临时消息相关 ---- */
static lv_obj_t *s_temp_msg_box = NULL;
static lv_timer_t *s_temp_msg_timer = NULL;

static void temp_msg_timer_cb(lv_timer_t *timer)
{
    if (s_temp_msg_box != NULL) {
        lv_obj_del(s_temp_msg_box);
        s_temp_msg_box = NULL;
    }
    if (s_temp_msg_timer != NULL) {
        lv_timer_del(s_temp_msg_timer);
        s_temp_msg_timer = NULL;
    }
}

void ui_show_temporary_message(const char *message, int duration_seconds)
{
    if (message == NULL || s_screen == NULL) return;

    /* 如果已有临时消息，先删除 */
    if (s_temp_msg_box != NULL) {
        lv_obj_del(s_temp_msg_box);
        s_temp_msg_box = NULL;
    }
    if (s_temp_msg_timer != NULL) {
        lv_timer_del(s_temp_msg_timer);
        s_temp_msg_timer = NULL;
    }

    /* 创建消息框：半透明黑色背景 */
    s_temp_msg_box = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_temp_msg_box);
    lv_obj_set_size(s_temp_msg_box, BSP_TFT_GetWidth() - 16, 50);
    lv_obj_center(s_temp_msg_box);
    lv_obj_set_style_radius(s_temp_msg_box, 8, 0);
    lv_obj_set_style_bg_opa(s_temp_msg_box, LV_OPA_80, 0);
    lv_obj_set_style_bg_color(s_temp_msg_box, lv_color_black(), 0);
    lv_obj_set_style_border_width(s_temp_msg_box, 2, 0);
    lv_obj_set_style_border_color(s_temp_msg_box, lv_color_white(), 0);
    lv_obj_set_style_pad_all(s_temp_msg_box, 8, 0);

    /* 创建消息文本 */
    lv_obj_t *label = lv_label_create(s_temp_msg_box);
    lv_label_set_text(label, message);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_font(label, font_mid, 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(label, BSP_TFT_GetWidth() - 32);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_center(label);

    /* 设置定时器自动删除 */
    if (duration_seconds > 0) {
        s_temp_msg_timer = lv_timer_create(temp_msg_timer_cb, duration_seconds * 1000, NULL);
        lv_timer_set_repeat_count(s_temp_msg_timer, 1);
    }
}
