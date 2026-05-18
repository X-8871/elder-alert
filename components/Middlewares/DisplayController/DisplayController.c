#include "DisplayController.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "BSP_TFT.h"
#include "WiFiManager.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lvgl.h"

static const char *TAG = "DisplayController";

#define UI_TOP_BAR_HEIGHT           22
#define UI_TOP_BAR_PAD_HOR          6
#define UI_TOP_BAR_PAD_VER          2
#define UI_TIME_LABEL_WIDTH         52
#define UI_WIFI_SLASH_SIZE          14
#define UI_OFFLINE_BANNER_Y         22
#define UI_TEXT_WIDTH               148
#define UI_STATE_TEXT_WIDTH         148
#define UI_STATUS_AREA_HEIGHT       70
#define UI_STATE_LABEL_OFFSET_Y     -4
#define UI_BOTTOM_PANEL_HEIGHT      38
#define UI_BOTTOM_PANEL_Y           (BSP_TFT_GetHeight() - UI_BOTTOM_PANEL_HEIGHT)
#define UI_DIVIDER_HEIGHT           2
#define UI_BUTTON_WIDTH             64
#define UI_BUTTON_HEIGHT            26
#define UI_BUTTON_BOTTOM_MARGIN     6
#define UI_BUTTON_SIDE_MARGIN       10
#define UI_BUTTON_RADIUS            8
#define UI_BUTTON_PAD               4

static bool s_enabled = false;
static lv_display_t *s_display = NULL;
static void *s_buf1 = NULL;
static void *s_buf2 = NULL;

static lv_obj_t *s_screen = NULL;
static lv_obj_t *s_top_bar = NULL;
static lv_obj_t *s_time_label = NULL;
static lv_obj_t *s_network_label = NULL;
static lv_obj_t *s_network_slash_line = NULL;
static lv_obj_t *s_offline_banner = NULL;
static lv_obj_t *s_status_area = NULL;
static lv_obj_t *s_state_label = NULL;
static lv_obj_t *s_divider = NULL;
static lv_obj_t *s_bottom_panel = NULL;
static lv_obj_t *s_confirm_button = NULL;
static lv_obj_t *s_sos_button = NULL;
static lv_obj_t *s_confirm_label = NULL;
static lv_obj_t *s_sos_label = NULL;

static const lv_point_precise_t s_wifi_slash_points[] = {
    {1, 12},
    {12, 1},
};

typedef struct {
    const char *state_text;
    lv_color_t bg_color;
} display_state_style_t;

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
    return LV_FONT_DEFAULT;
}

static display_state_style_t display_state_style(app_state_t state)
{
    switch (state) {
    case APP_STATE_NORMAL:
        return (display_state_style_t){
            .state_text = "NORMAL",
            .bg_color = lv_color_hex(0x1E9E55),
        };
    case APP_STATE_REMIND:
        return (display_state_style_t){
            .state_text = "REMIND",
            .bg_color = lv_color_hex(0xE58A1F),
        };
    case APP_STATE_ALARM:
        return (display_state_style_t){
            .state_text = "ALARM",
            .bg_color = lv_color_hex(0xD64633),
        };
    case APP_STATE_SOS:
        return (display_state_style_t){
            .state_text = "SOS",
            .bg_color = lv_color_hex(0xA61E22),
        };
    default:
        return (display_state_style_t){
            .state_text = "UNKNOWN",
            .bg_color = lv_color_hex(0x5E5E5E),
        };
    }
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
    lv_obj_set_style_radius(obj, UI_BUTTON_RADIUS, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(obj, bg_color, 0);
    lv_obj_set_style_pad_all(obj, UI_BUTTON_PAD, 0);
}

static lv_obj_t *create_center_label(lv_obj_t *parent,
                                     uint16_t width,
                                     lv_color_t text_color,
                                     lv_align_t align,
                                     int32_t y_offset)
{
    lv_obj_t *label = lv_label_create(parent);
    if (label == NULL) {
        return NULL;
    }

    lv_obj_set_width(label, width);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(label, ui_font(), 0);
    lv_obj_set_style_text_color(label, text_color, 0);
    lv_obj_align(label, align, 0, y_offset);

    return label;
}

static lv_obj_t *create_button_label(lv_obj_t *button, const char *text)
{
    lv_obj_t *label = lv_label_create(button);
    if (label == NULL) {
        return NULL;
    }

    lv_obj_set_style_text_font(label, ui_font(), 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_label_set_text(label, text);
    lv_obj_center(label);

    return label;
}

static const lv_font_t *ui_state_font(void)
{
    return &lv_font_montserrat_20;
}

static esp_err_t create_top_bar(void)
{
    s_top_bar = lv_obj_create(s_screen);
    if (s_top_bar == NULL) {
        return ESP_ERR_NO_MEM;
    }

    lv_obj_remove_style_all(s_top_bar);
    lv_obj_set_size(s_top_bar, BSP_TFT_GetWidth(), UI_TOP_BAR_HEIGHT);
    lv_obj_align(s_top_bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_opa(s_top_bar, LV_OPA_90, 0);
    lv_obj_set_style_bg_color(s_top_bar, lv_color_white(), 0);
    lv_obj_set_style_pad_hor(s_top_bar, UI_TOP_BAR_PAD_HOR, 0);
    lv_obj_set_style_pad_ver(s_top_bar, UI_TOP_BAR_PAD_VER, 0);

    s_time_label = lv_label_create(s_top_bar);
    if (s_time_label == NULL) {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_set_width(s_time_label, UI_TIME_LABEL_WIDTH);
    lv_label_set_long_mode(s_time_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(s_time_label, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_font(s_time_label, ui_font(), 0);
    lv_obj_set_style_text_color(s_time_label, lv_color_hex(0x1E1E1E), 0);
    lv_obj_align(s_time_label, LV_ALIGN_LEFT_MID, 0, 0);

    s_network_label = lv_label_create(s_top_bar);
    if (s_network_label == NULL) {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_set_style_text_font(s_network_label, ui_font(), 0);
    lv_obj_set_style_text_color(s_network_label, lv_color_hex(0x1E1E1E), 0);
    lv_obj_align(s_network_label, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_label_set_text(s_network_label, LV_SYMBOL_WIFI);

    s_network_slash_line = lv_line_create(s_top_bar);
    if (s_network_slash_line == NULL) {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_set_size(s_network_slash_line, UI_WIFI_SLASH_SIZE, UI_WIFI_SLASH_SIZE);
    lv_line_set_points(s_network_slash_line, s_wifi_slash_points, 2);
    lv_obj_set_style_line_width(s_network_slash_line, 2, 0);
    lv_obj_set_style_line_color(s_network_slash_line, lv_color_black(), 0);
    lv_obj_set_style_line_rounded(s_network_slash_line, true, 0);
    lv_obj_align_to(s_network_slash_line, s_network_label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(s_network_slash_line, LV_OBJ_FLAG_HIDDEN);

    return ESP_OK;
}

static esp_err_t create_status_labels(void)
{
    s_offline_banner = create_center_label(s_screen,
                                           UI_TEXT_WIDTH,
                                           lv_color_white(),
                                           LV_ALIGN_TOP_MID,
                                           UI_OFFLINE_BANNER_Y);
    if (s_offline_banner == NULL) {
        return ESP_ERR_NO_MEM;
    }
    lv_label_set_text(s_offline_banner, "OFFLINE MODE");

    s_status_area = lv_obj_create(s_screen);
    if (s_status_area == NULL) {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_remove_style_all(s_status_area);
    lv_obj_set_size(s_status_area, BSP_TFT_GetWidth(), UI_STATUS_AREA_HEIGHT);
    lv_obj_align(s_status_area, LV_ALIGN_CENTER, 0, -UI_BOTTOM_PANEL_HEIGHT / 2);

    s_state_label = create_center_label(s_status_area,
                                        UI_STATE_TEXT_WIDTH,
                                        lv_color_white(),
                                        LV_ALIGN_CENTER,
                                        UI_STATE_LABEL_OFFSET_Y);
    if (s_state_label == NULL) {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_set_style_text_font(s_state_label, ui_state_font(), 0);
    lv_label_set_long_mode(s_state_label, LV_LABEL_LONG_CLIP);
    return ESP_OK;
}

static esp_err_t create_button_row(void)
{
    s_divider = lv_obj_create(s_screen);
    if (s_divider == NULL) {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_remove_style_all(s_divider);
    lv_obj_set_size(s_divider, BSP_TFT_GetWidth(), UI_DIVIDER_HEIGHT);
    lv_obj_align(s_divider, LV_ALIGN_TOP_MID, 0, UI_BOTTOM_PANEL_Y - UI_DIVIDER_HEIGHT);
    lv_obj_set_style_bg_opa(s_divider, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_divider, lv_color_hex(0xFFFFFF), 0);

    s_bottom_panel = lv_obj_create(s_screen);
    if (s_bottom_panel == NULL) {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_remove_style_all(s_bottom_panel);
    lv_obj_set_size(s_bottom_panel, BSP_TFT_GetWidth(), UI_BOTTOM_PANEL_HEIGHT);
    lv_obj_align(s_bottom_panel, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(s_bottom_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_bottom_panel, lv_color_white(), 0);

    s_confirm_button = lv_obj_create(s_screen);
    if (s_confirm_button == NULL) {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_remove_style_all(s_confirm_button);
    lv_obj_set_size(s_confirm_button, UI_BUTTON_WIDTH, UI_BUTTON_HEIGHT);
    lv_obj_align_to(s_confirm_button, s_bottom_panel, LV_ALIGN_LEFT_MID, UI_BUTTON_SIDE_MARGIN, 0);
    style_button_box(s_confirm_button, lv_color_hex(0x147A46));

    s_confirm_label = create_button_label(s_confirm_button, "Confirm");
    if (s_confirm_label == NULL) {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_set_style_text_font(s_confirm_label, LV_FONT_DEFAULT, 0);

    s_sos_button = lv_obj_create(s_screen);
    if (s_sos_button == NULL) {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_remove_style_all(s_sos_button);
    lv_obj_set_size(s_sos_button, UI_BUTTON_WIDTH, UI_BUTTON_HEIGHT);
    lv_obj_align_to(s_sos_button, s_bottom_panel, LV_ALIGN_RIGHT_MID, -UI_BUTTON_SIDE_MARGIN, 0);
    style_button_box(s_sos_button, lv_color_hex(0xC92F2A));

    s_sos_label = create_button_label(s_sos_button, "SOS");
    if (s_sos_label == NULL) {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_set_style_text_font(s_sos_label, LV_FONT_DEFAULT, 0);

    return ESP_OK;
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

    esp_err_t ret = create_top_bar();
    if (ret != ESP_OK) {
        return ret;
    }

    ret = create_status_labels();
    if (ret != ESP_OK) {
        return ret;
    }

    ret = create_button_row();
    if (ret != ESP_OK) {
        return ret;
    }

    lv_screen_load(s_screen);
    return ESP_OK;
}

static void update_online_styles(bool online)
{
    const lv_color_t top_bar_color = lv_color_white();
    const lv_color_t text_color = lv_color_hex(0x1E1E1E);

    lv_obj_set_style_bg_color(s_top_bar, top_bar_color, 0);
    lv_obj_set_style_text_color(s_time_label, text_color, 0);
    lv_obj_set_style_text_color(s_network_label, text_color, 0);
    lv_obj_add_flag(s_offline_banner, LV_OBJ_FLAG_HIDDEN);

    if (online) {
        lv_obj_add_flag(s_network_slash_line, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(s_network_slash_line, LV_OBJ_FLAG_HIDDEN);
    }
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

    const bool online = WiFiManager_IsConnected();
    const display_state_style_t style = display_state_style(app_state);

    lv_obj_set_style_bg_color(s_screen, online ? style.bg_color : lv_color_hex(0x6F7680), 0);
    update_online_styles(online);

    lv_label_set_text(s_time_label, time_text);
    lv_label_set_text(s_network_label, LV_SYMBOL_WIFI);
    lv_label_set_text(s_state_label, online ? style.state_text : "OFFLINE");
    lv_obj_move_foreground(s_time_label);
    lv_obj_move_foreground(s_network_label);
    lv_obj_move_foreground(s_network_slash_line);

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
