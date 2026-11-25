#include <rtthread.h>
#include <stdio.h>
#include <time.h>
#include "ui_manager.h"
#include "watch_info.h"
#include "ui_weather.h"
#include "uvc_ai_app.h"
#include "alert_manager.h"
#include "wifi_stream.h"
#include "xiaozhi.h"

extern float g_aht_temp;
extern float g_aht_humi;

static ui_chrome_t face_chrome, apps_chrome;
static lv_obj_t *seconds_ring, *clock_label, *topbar_hms_label, *date_label;
static lv_obj_t *weather_label, *cabin_label, *monitor_label, *gemini_sub_label;

extern const unsigned char watch_wallpaper_rgb565[];
extern const unsigned char gemini_avatar_64_rgb565[];
static const lv_image_dsc_t wallpaper = {
    .header = { .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565,
                .w = UI_SCREEN_W, .h = UI_SCREEN_H, .stride = UI_SCREEN_W * 2 },
    .data_size = UI_SCREEN_W * UI_SCREEN_H * 2,
    .data = watch_wallpaper_rgb565,
};
static const lv_image_dsc_t gemini_mini_avatar = {
    .header = { .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565,
                .w = 64, .h = 64, .stride = 64 * 2 },
    .data_size = 64 * 64 * 2,
    .data = gemini_avatar_64_rgb565,
};

static lv_obj_t *make_label(lv_obj_t *parent, const char *text, const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, color, LV_PART_MAIN);
    return label;
}

static void open_page(lv_event_t *event)
{
    ui_switch_to((ui_screen_id_t)(intptr_t)lv_event_get_user_data(event));
}

static void open_ai_mode(lv_event_t *event)
{
    uvc_ai_mode_t mode = (uvc_ai_mode_t)(intptr_t)lv_event_get_user_data(event);
    (void)uvc_ai_app_set_mode(mode);
    ui_switch_to(UI_SCR_FATIGUE);
}

static void open_clock_tab(lv_event_t *event)
{
    int tab = (int)(intptr_t)lv_event_get_user_data(event);
    ui_screen_clock_tools_open_tab(tab);
    ui_switch_to(UI_SCR_CLOCK_TOOLS);
}

static lv_obj_t *make_tile(lv_obj_t *parent, const char *name, const char *initial)
{
    lv_obj_t *tile = lv_obj_create(parent);
    lv_obj_add_style(tile, &g_st_card, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(tile, LV_OPA_90, LV_PART_MAIN);
    lv_obj_set_size(tile, 222, 105);
    lv_obj_remove_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(tile, 8, LV_PART_MAIN);
    make_label(tile, name, UI_FONT_ZH, UI_COL_MUTED);
    lv_obj_t *value = make_label(tile, initial, UI_FONT_ZH, UI_COL_TEXT);
    lv_obj_set_width(value, lv_pct(100));
    return value;
}

lv_obj_t *ui_screen_home_create(void)
{
    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_t *background = lv_image_create(screen);
    lv_image_set_src(background, &wallpaper);
    lv_obj_set_pos(background, 0, 0);
    lv_obj_remove_flag(background, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    ui_create_chrome(screen, "EDGI 表盘", UI_SCR_HOME, &face_chrome);

    /* Sleek digital HH:MM:SS pill right next to "EDGI 表盘" in top bar (tap -> Countdown) */
    lv_obj_t *hms_pill = lv_obj_create(face_chrome.topbar);
    lv_obj_add_style(hms_pill, &g_st_pill, LV_PART_MAIN);
    lv_obj_add_flag(hms_pill, LV_OBJ_FLAG_IGNORE_LAYOUT | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(hms_pill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(hms_pill, LV_SIZE_CONTENT, 28);
    lv_obj_set_style_pad_hor(hms_pill, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(hms_pill, 3, LV_PART_MAIN);
    lv_obj_set_style_border_color(hms_pill, UI_COL_CYAN, LV_PART_MAIN);
    lv_obj_set_style_bg_color(hms_pill, lv_color_hex(0x10233A), LV_PART_MAIN);
    lv_obj_align(hms_pill, LV_ALIGN_LEFT_MID, 118, 0);
    lv_obj_add_event_cb(hms_pill, open_clock_tab, LV_EVENT_CLICKED, (void *)(intptr_t)2);
    topbar_hms_label = make_label(hms_pill, "--:--:--", &lv_font_montserrat_16, UI_COL_CYAN);
    lv_obj_center(topbar_hms_label);

    lv_obj_t *body = face_chrome.body;
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(body, 0, LV_PART_MAIN);

    /* Clock Card (tap date/clock -> Calendar; tap seconds ring -> Stopwatch) */
    lv_obj_t *clock_card = lv_obj_create(body);
    lv_obj_set_pos(clock_card, 12, 16);
    lv_obj_set_size(clock_card, 456, 112);
    lv_obj_add_style(clock_card, &g_st_card, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(clock_card, LV_OPA_80, LV_PART_MAIN);
    lv_obj_remove_flag(clock_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(clock_card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(clock_card, open_clock_tab, LV_EVENT_CLICKED, (void *)(intptr_t)0);

    seconds_ring = lv_arc_create(clock_card);
    lv_obj_set_size(seconds_ring, 64, 64);
    lv_obj_align(seconds_ring, LV_ALIGN_RIGHT_MID, -10, 0);
    lv_arc_set_bg_angles(seconds_ring, 0, 360);
    lv_arc_set_rotation(seconds_ring, 270);
    lv_arc_set_range(seconds_ring, 0, 60);
    lv_arc_set_value(seconds_ring, 0);
    lv_obj_remove_style(seconds_ring, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(seconds_ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(seconds_ring, 5, LV_PART_MAIN);
    lv_obj_set_style_arc_width(seconds_ring, 5, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(seconds_ring, UI_COL_BORDER, LV_PART_MAIN);
    lv_obj_set_style_arc_color(seconds_ring, UI_COL_CYAN, LV_PART_INDICATOR);

    /* Transparent tap hotspot over seconds_ring -> Stopwatch (preserves exact appearance) */
    lv_obj_t *ring_hit = lv_obj_create(clock_card);
    lv_obj_remove_style_all(ring_hit);
    lv_obj_set_size(ring_hit, 76, 76);
    lv_obj_align(ring_hit, LV_ALIGN_RIGHT_MID, -4, 0);
    lv_obj_add_flag(ring_hit, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(ring_hit, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(ring_hit, open_clock_tab, LV_EVENT_CLICKED, (void *)(intptr_t)1);

    clock_label = make_label(clock_card, "--:--", &lv_font_montserrat_48, UI_COL_TEXT);
    lv_obj_align(clock_label, LV_ALIGN_LEFT_MID, 9, -13);
    date_label = make_label(clock_card, "时间待同步", UI_FONT_ZH, UI_COL_MUTED);
    lv_obj_align(date_label, LV_ALIGN_BOTTOM_LEFT, 9, 0);

    lv_obj_t *tiles = lv_obj_create(body);
    lv_obj_remove_style_all(tiles);
    lv_obj_set_size(tiles, 456, 105);
    lv_obj_set_pos(tiles, 12, 508);
    lv_obj_remove_flag(tiles, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_column(tiles, 12, LV_PART_MAIN);
    lv_obj_set_flex_flow(tiles, LV_FLEX_FLOW_ROW);
    weather_label = make_tile(tiles, "室外天气", "等待同步");
    lv_obj_t *weather_tile = lv_obj_get_parent(weather_label);
    lv_obj_add_flag(weather_tile, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(weather_tile, open_page, LV_EVENT_CLICKED, (void *)(intptr_t)UI_SCR_WEATHER);
    cabin_label = make_tile(tiles, "车内环境", "--");
    lv_obj_t *cabin_tile = lv_obj_get_parent(cabin_label);
    lv_obj_add_flag(cabin_tile, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(cabin_tile, open_page, LV_EVENT_CLICKED, (void *)(intptr_t)UI_SCR_WEATHER);

    lv_obj_t *monitor = lv_button_create(body);
    lv_obj_add_style(monitor, &g_st_btn_primary, LV_PART_MAIN);
    lv_obj_set_size(monitor, 222, 64);
    lv_obj_set_pos(monitor, 12, 620);
    lv_obj_add_event_cb(monitor, open_ai_mode, LV_EVENT_CLICKED, (void *)(intptr_t)UVC_AI_MODE_FATIGUE);
    monitor_label = make_label(monitor, LV_SYMBOL_EYE_OPEN "  疲劳监测", UI_FONT_ZH,
                               lv_color_hex(0x06101F));
    lv_obj_center(monitor_label);

    /* Gemini-chan XiaoZhi AI Quick Entry Button */
    lv_obj_t *gemini_btn = lv_button_create(body);
    lv_obj_set_size(gemini_btn, 222, 64);
    lv_obj_set_pos(gemini_btn, 246, 620);
    lv_obj_set_style_radius(gemini_btn, 12, LV_PART_MAIN);
    lv_obj_set_style_bg_color(gemini_btn, UI_COL_GEMINI_VIOLET, LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(gemini_btn, lv_color_hex(0x3B82F6), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(gemini_btn, LV_GRAD_DIR_HOR, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(gemini_btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(gemini_btn, UI_COL_GEMINI_PURPLE, LV_PART_MAIN);
    lv_obj_set_style_border_width(gemini_btn, 1, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(gemini_btn, 10, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(gemini_btn, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(gemini_btn, open_page, LV_EVENT_CLICKED, (void *)(intptr_t)UI_SCR_XIAOZHI);

    lv_obj_t *av_wrap = lv_obj_create(gemini_btn);
    lv_obj_remove_style_all(av_wrap);
    lv_obj_set_size(av_wrap, 48, 48);
    lv_obj_align(av_wrap, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_radius(av_wrap, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_clip_corner(av_wrap, true, LV_PART_MAIN);
    lv_obj_set_style_border_color(av_wrap, UI_COL_GEMINI_PINK, LV_PART_MAIN);
    lv_obj_set_style_border_width(av_wrap, 2, LV_PART_MAIN);
    lv_obj_remove_flag(av_wrap, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *av_img = lv_image_create(av_wrap);
    lv_image_set_src(av_img, &gemini_mini_avatar);
    lv_image_set_scale(av_img, 192);
    lv_obj_center(av_img);

    lv_obj_t *g_title = make_label(gemini_btn, "Gemini娘", ui_font_zh_full(), UI_COL_TEXT);
    lv_obj_align(g_title, LV_ALIGN_LEFT_MID, 58, -11);
    gemini_sub_label = make_label(gemini_btn, "小智语音", ui_font_zh_small(), lv_color_hex(0xE0E7FF));
    lv_obj_align(gemini_sub_label, LV_ALIGN_LEFT_MID, 58, 12);

    ui_screen_home_update();
    return screen;
}

void ui_screen_home_update(void)
{
    watch_info_t info;
    uvc_ai_telemetry_t telemetry;
    time_t now = time(NULL);
    char text[64];
    struct tm date;
    ui_update_chrome(&face_chrome);
    watch_info_get(&info);
    const time_t local_epoch = now + info.offset_minutes * 60;
    if (now >= (time_t)WATCH_EPOCH_MIN && gmtime_r(&local_epoch, &date) != NULL)
    {
        snprintf(text, sizeof(text), "%02d:%02d", date.tm_hour, date.tm_min);
        ui_set_label_text(clock_label, text);

        snprintf(text, sizeof(text), "%02d:%02d:%02d", date.tm_hour, date.tm_min, date.tm_sec);
        ui_set_label_text(topbar_hms_label, text);

        static const char *const weekdays[] = {"日", "一", "二", "三", "四", "五", "六"};
        snprintf(text, sizeof(text), "%d月%d日  星期%s", date.tm_mon + 1, date.tm_mday,
                 weekdays[date.tm_wday]);
        ui_set_label_text(date_label, text);
        lv_arc_set_value(seconds_ring, date.tm_sec);
    }

    if (ui_weather_is_fresh(&info, now))
        snprintf(text, sizeof(text), "%s %.1f ℃\n%s", ui_weather_city(info.city),
                 info.temperature_tenths / 10.0, ui_weather_condition(info.weather_code));
    else if (info.weather_code >= 0)
        snprintf(text, sizeof(text), "%s %.1f ℃\n待更新", ui_weather_city(info.city),
                 info.temperature_tenths / 10.0);
    else
        snprintf(text, sizeof(text), "%s · 等待同步", ui_weather_city(info.city));
    ui_set_label_text(weather_label, text);

    snprintf(text, sizeof(text), "%.1f ℃   %.0f%%", (double)g_aht_temp, (double)g_aht_humi);
    ui_set_label_text(cabin_label, text);
    uvc_ai_app_get_telemetry(&telemetry);
    ui_set_label_text(monitor_label, telemetry.alert_active ? LV_SYMBOL_WARNING "  疲劳警报" :
                      LV_SYMBOL_EYE_OPEN "  疲劳监测");

    if (gemini_sub_label != NULL)
    {
        xz_ui_snapshot_t xzs;
        xiaozhi_get_snapshot(&xzs);
        ui_set_label_text(gemini_sub_label, xzs.ws_connected ? "在线 · 点击聊" : "小智语音");
    }
}

static void test_alarm(lv_event_t *event) { (void)event; (void)alert_test_sound(); }
static void snapshot(lv_event_t *event) { (void)event; (void)wifi_stream_push_snapshot("cockpit_snap.jpg"); }

lv_obj_t *ui_screen_apps_create(void)
{
    static const struct
    {
        const char *icon;
        const char *name;
        lv_event_cb_t event;
        intptr_t arg;
        bool use_full_font;
    } items[] = {
        {LV_SYMBOL_AUDIO,    "小智 AI",  open_page,      (intptr_t)UI_SCR_XIAOZHI,    true},
        {LV_SYMBOL_EYE_OPEN, "疲劳监测", open_ai_mode,   (intptr_t)UVC_AI_MODE_FATIGUE, false},
        {LV_SYMBOL_EYE_OPEN, "钢球监测", open_ai_mode,   (intptr_t)UVC_AI_MODE_STEEL_BALL, true},
        {LV_SYMBOL_TINT,     "天气",     open_page,      (intptr_t)UI_SCR_WEATHER,    false},
        {LV_SYMBOL_LIST,     "日历",     open_clock_tab, 0,                           true},
        {LV_SYMBOL_PLAY,     "计时器",   open_clock_tab, 1,                           true},
        {LV_SYMBOL_BELL,     "倒计时",   open_clock_tab, 2,                           true},
        {LV_SYMBOL_WIFI,     "无线网络", open_page,      (intptr_t)UI_SCR_WIFI,       false},
        {LV_SYMBOL_SETTINGS, "系统",     open_page,      (intptr_t)UI_SCR_SYSTEM,     false},
        {LV_SYMBOL_IMAGE,    "图库",     open_page,      (intptr_t)UI_SCR_GALLERY,    false},
        {LV_SYMBOL_CHARGE,   "亮度",     open_page,      (intptr_t)UI_SCR_BRIGHTNESS, false},
        {LV_SYMBOL_VIDEO,    "相机",     open_page,      (intptr_t)UI_SCR_CAMERA,     false},
        {LV_SYMBOL_AUDIO,    "试听警报", test_alarm,     0,                           false},
        {LV_SYMBOL_IMAGE,    "抓拍",     snapshot,       0,                           false},
    };
    lv_obj_t *screen = lv_obj_create(NULL);
    ui_create_chrome(screen, "应用", UI_SCR_APPS, &apps_chrome);

    lv_obj_t *body = apps_chrome.body;
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_pad_gap(body, 12, LV_PART_MAIN);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    for (size_t i = 0; i < sizeof(items) / sizeof(items[0]); i++)
    {
        lv_obj_t *tile = lv_button_create(body);
        lv_obj_add_style(tile, &g_st_btn_ghost, LV_PART_MAIN);
        lv_obj_set_size(tile, 144, 152);
        bool is_xz = (items[i].event == open_page && items[i].arg == (intptr_t)UI_SCR_XIAOZHI);
        if (is_xz)
        {
            lv_obj_set_style_border_color(tile, UI_COL_GEMINI_PURPLE, LV_PART_MAIN);
            lv_obj_set_style_bg_color(tile, lv_color_hex(0x231A45), LV_PART_MAIN);
        }
        lv_obj_add_event_cb(tile, items[i].event, LV_EVENT_CLICKED, (void *)items[i].arg);

        lv_obj_t *icon = make_label(tile, items[i].icon, &lv_font_montserrat_32,
                                    is_xz ? UI_COL_GEMINI_PINK : UI_COL_CYAN);
        lv_obj_align(icon, LV_ALIGN_CENTER, 0, -22);
        lv_obj_t *name = make_label(tile, items[i].name,
                                    items[i].use_full_font ? ui_font_zh_full() : UI_FONT_ZH,
                                    UI_COL_TEXT);
        lv_obj_align(name, LV_ALIGN_CENTER, 0, 30);
    }
    return screen;
}

void ui_screen_apps_update(void)
{
    ui_update_chrome(&apps_chrome);
}
