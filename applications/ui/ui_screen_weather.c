#include <stdio.h>
#include "ui_manager.h"
#include "ui_weather.h"

static ui_chrome_t chrome;
static lv_obj_t *city_label, *temperature_label, *condition_label;
static lv_obj_t *apparent_label, *humidity_label, *status_label, *time_label;

const char *ui_weather_city(int city)
{
    static const char *const cities[] = {"上海", "北京", "深圳", "自定义地点"};
    return city >= 0 && city < (int)(sizeof(cities) / sizeof(cities[0])) ? cities[city] : cities[0];
}

const char *ui_weather_condition(int code)
{
    if (code < 0) return "等待天气数据";
    if (code == 0) return "晴";
    if (code <= 2) return "多云";
    if (code == 3) return "阴";
    if (code <= 48) return "雾";
    if (code <= 57) return "毛毛雨";
    if (code <= 67) return "雨";
    if (code <= 77) return "雪";
    if (code <= 82) return "阵雨";
    if (code <= 86) return "阵雪";
    return "雷暴";
}

bool ui_weather_is_fresh(const watch_info_t *info, time_t now)
{
    return info->weather_code >= 0 && now >= info->weather_epoch &&
           now - info->weather_epoch <= WATCH_WEATHER_MAX_AGE;
}

static lv_obj_t *weather_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, "");
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, color, LV_PART_MAIN);
    return label;
}

static lv_obj_t *card(lv_obj_t *parent, int width, int height)
{
    lv_obj_t *object = lv_obj_create(parent);
    lv_obj_set_size(object, width, height);
    lv_obj_add_style(object, &g_st_card, LV_PART_MAIN);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
    return object;
}

static lv_obj_t *metric(lv_obj_t *parent, const char *name)
{
    lv_obj_t *tile = card(parent, 222, 112);
    lv_obj_t *title = weather_label(tile, UI_FONT_ZH, UI_COL_MUTED);
    lv_label_set_text(title, name);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 6, 2);
    lv_obj_t *value = weather_label(tile, UI_FONT_ZH, UI_COL_TEXT);
    lv_obj_align(value, LV_ALIGN_BOTTOM_LEFT, 6, -6);
    return value;
}

lv_obj_t *ui_screen_weather_create(void)
{
    lv_obj_t *screen = lv_obj_create(NULL);
    ui_create_chrome(screen, "天气", UI_SCR_WEATHER, &chrome);
    lv_obj_t *body = chrome.body;
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(body, 14, LV_PART_MAIN);

    lv_obj_t *hero = card(body, 456, 282);
    lv_obj_set_style_bg_color(hero, lv_color_hex(0x102842), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(hero, lv_color_hex(0x18243E), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(hero, LV_GRAD_DIR_VER, LV_PART_MAIN);
    city_label = weather_label(hero, UI_FONT_ZH, UI_COL_CYAN);
    lv_obj_align(city_label, LV_ALIGN_TOP_MID, 0, 24);
    temperature_label = weather_label(hero, &lv_font_montserrat_48, UI_COL_TEXT);
    lv_obj_align(temperature_label, LV_ALIGN_CENTER, 0, -6);
    condition_label = weather_label(hero, UI_FONT_ZH, UI_COL_TEXT);
    lv_obj_align(condition_label, LV_ALIGN_BOTTOM_MID, 0, -28);

    lv_obj_t *metrics = lv_obj_create(body);
    lv_obj_remove_style_all(metrics);
    lv_obj_set_size(metrics, 456, 112);
    lv_obj_remove_flag(metrics, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_column(metrics, 12, LV_PART_MAIN);
    lv_obj_set_flex_flow(metrics, LV_FLEX_FLOW_ROW);
    apparent_label = metric(metrics, "体感温度");
    humidity_label = metric(metrics, "相对湿度");

    lv_obj_t *source = card(body, 456, 108);
    status_label = weather_label(source, UI_FONT_ZH, UI_COL_CYAN);
    lv_obj_align(status_label, LV_ALIGN_TOP_LEFT, 6, 5);
    time_label = weather_label(source, UI_FONT_ZH, UI_COL_MUTED);
    lv_obj_align(time_label, LV_ALIGN_BOTTOM_LEFT, 6, -5);
    ui_screen_weather_update();
    return screen;
}

void ui_screen_weather_update(void)
{
    watch_info_t info;
    char text[64];
    ui_update_chrome(&chrome);
    watch_info_get(&info);
    time_t now = time(NULL);
    ui_set_label_text(city_label, ui_weather_city(info.city));
    ui_set_label_text(condition_label, ui_weather_condition(info.weather_code));

    if (info.weather_code < 0)
    {
        ui_set_label_text(temperature_label, "--");
        ui_set_label_text(apparent_label, "--");
        ui_set_label_text(humidity_label, "--");
        ui_set_label_text(status_label, "等待同步");
        ui_set_label_text(time_label, "可通过电脑串口或局域网更新");
        return;
    }

    snprintf(text, sizeof(text), "%.1f°", info.temperature_tenths / 10.0);
    ui_set_label_text(temperature_label, text);
    snprintf(text, sizeof(text), "%.1f ℃", info.apparent_tenths / 10.0);
    ui_set_label_text(apparent_label, info.humidity < 0 ? "--" : text);
    snprintf(text, sizeof(text), "%d%%", info.humidity);
    ui_set_label_text(humidity_label, info.humidity < 0 ? "--" : text);
    const char *source = info.source == WATCH_WEATHER_USB ? "电脑串口" : "局域网";
    snprintf(text, sizeof(text), "%s  ·  %s", source,
             ui_weather_is_fresh(&info, now) ? "天气实况" : "数据已过期");
    ui_set_label_text(status_label, text);
    struct tm local;
    time_t at = info.weather_epoch + info.offset_minutes * 60;
    if (gmtime_r(&at, &local) != NULL)
    {
        snprintf(text, sizeof(text), "观测时间  %02d-%02d  %02d:%02d",
                 local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min);
        ui_set_label_text(time_label, text);
    }
}
