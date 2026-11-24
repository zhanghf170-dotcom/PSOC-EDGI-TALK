#include <rtthread.h>
#include "ui_manager.h"

extern rt_err_t lcd_set_brightness(rt_uint8_t percent);
extern rt_uint8_t lcd_get_brightness(void);

static ui_chrome_t brightness_chrome;

static void brightness_sync(lv_obj_t *slider)
{
    unsigned percent = lcd_get_brightness();
    lv_obj_t *label = lv_obj_get_user_data(slider);
    lv_slider_set_value(slider, (int32_t)percent, LV_ANIM_OFF);
    lv_label_set_text_fmt(label, "亮度  %u%%", percent);
}

static void brightness_loaded(lv_event_t *event)
{
    brightness_sync(lv_event_get_user_data(event));
}

static void brightness_changed(lv_event_t *event)
{
    lv_obj_t *slider = lv_event_get_target(event);
    rt_err_t result = lcd_set_brightness((rt_uint8_t)lv_slider_get_value(slider));
    brightness_sync(slider);
    if (result != RT_EOK)
        lv_label_set_text(lv_obj_get_user_data(slider), "亮度调节失败");
}

lv_obj_t *ui_brightness_slider_create(lv_obj_t *parent, lv_obj_t *value_label)
{
    lv_obj_t *slider = lv_slider_create(parent);
    lv_obj_set_user_data(slider, value_label);
    lv_slider_set_range(slider, 15, 100);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0x1E293B), LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, UI_COL_CYAN, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, UI_COL_TEXT, LV_PART_KNOB);
    brightness_sync(slider);
    lv_obj_add_event_cb(slider, brightness_changed, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(lv_obj_get_screen(parent), brightness_loaded, LV_EVENT_SCREEN_LOADED, slider);
    return slider;
}

lv_obj_t *ui_screen_brightness_create(void)
{
    lv_obj_t *screen = lv_obj_create(NULL);
    ui_create_chrome(screen, "亮度", UI_SCR_APPS, &brightness_chrome);
    lv_obj_remove_flag(brightness_chrome.body, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *card = lv_obj_create(brightness_chrome.body);
    lv_obj_add_style(card, &g_st_card, LV_PART_MAIN);
    lv_obj_set_size(card, 456, 246);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *icon = lv_label_create(card);
    lv_label_set_text(icon, LV_SYMBOL_CHARGE);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_32, LV_PART_MAIN);
    lv_obj_set_style_text_color(icon, UI_COL_CYAN, LV_PART_MAIN);
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 18);

    lv_obj_t *value = lv_label_create(card);
    lv_obj_set_style_text_font(value, UI_FONT_ZH, LV_PART_MAIN);
    lv_obj_set_style_text_color(value, UI_COL_TEXT, LV_PART_MAIN);
    lv_obj_align(value, LV_ALIGN_TOP_MID, 0, 76);

    lv_obj_t *slider = ui_brightness_slider_create(card, value);
    lv_obj_set_size(slider, 366, 18);
    lv_obj_set_ext_click_area(slider, 16);
    lv_obj_align(slider, LV_ALIGN_TOP_MID, 0, 132);

    lv_obj_t *low = lv_label_create(card);
    lv_label_set_text(low, "15%");
    lv_obj_set_style_text_color(low, UI_COL_MUTED, LV_PART_MAIN);
    lv_obj_align(low, LV_ALIGN_TOP_LEFT, 28, 170);

    lv_obj_t *high = lv_label_create(card);
    lv_label_set_text(high, "100%");
    lv_obj_set_style_text_color(high, UI_COL_MUTED, LV_PART_MAIN);
    lv_obj_align(high, LV_ALIGN_TOP_RIGHT, -28, 170);

    return screen;
}

void ui_screen_brightness_update(void)
{
    ui_update_chrome(&brightness_chrome);
}
