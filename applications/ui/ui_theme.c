#include <rtthread.h>
#include "ui_theme.h"
#include "ui_manager.h"
#include "wifi_stream.h"
#include "dualcore_bridge.h"
#include <stdio.h>
#include <string.h>

lv_style_t g_st_screen;
lv_style_t g_st_card;
lv_style_t g_st_pill;
lv_style_t g_st_btn_primary;
lv_style_t g_st_btn_danger;
lv_style_t g_st_btn_ghost;

extern const unsigned char xiaozhi_zh_ttf[];
extern const size_t xiaozhi_zh_ttf_size;

static lv_font_t *g_font_zh_full = NULL;
static lv_font_t *g_font_zh_small = NULL;
static bool g_theme_inited = false;

const lv_font_t *ui_font_zh_full(void)
{
    return (g_font_zh_full != NULL) ? g_font_zh_full : UI_FONT_ZH;
}

const lv_font_t *ui_font_zh_small(void)
{
    return (g_font_zh_small != NULL) ? g_font_zh_small : UI_FONT_ZH;
}

void ui_set_label_text(lv_obj_t *label, const char *text)
{
    if (strcmp(lv_label_get_text(label), text) != 0)
        lv_label_set_text(label, text);
}

static void ui_set_text_color(lv_obj_t *label, lv_color_t color)
{
    if (!lv_color_eq(lv_obj_get_style_text_color(label, LV_PART_MAIN), color))
        lv_obj_set_style_text_color(label, color, LV_PART_MAIN);
}

static void nav_btn_cb(lv_event_t *e)
{
    ui_screen_id_t target = (ui_screen_id_t)(intptr_t)lv_event_get_user_data(e);
    ui_switch_to(target);
}

void ui_theme_init(void)
{
    if (g_theme_inited) return;

    lv_style_init(&g_st_screen);
    lv_style_set_bg_color(&g_st_screen, UI_COL_BG);
    lv_style_set_bg_opa(&g_st_screen, LV_OPA_COVER);
    lv_style_set_border_width(&g_st_screen, 0);
    lv_style_set_pad_all(&g_st_screen, 0);

    lv_style_init(&g_st_card);
    lv_style_set_bg_color(&g_st_card, UI_COL_CARD);
    lv_style_set_bg_opa(&g_st_card, LV_OPA_COVER);
    lv_style_set_border_color(&g_st_card, UI_COL_BORDER);
    lv_style_set_border_width(&g_st_card, 1);
    lv_style_set_radius(&g_st_card, 14);
    lv_style_set_pad_all(&g_st_card, 12);

    lv_style_init(&g_st_pill);
    lv_style_set_bg_color(&g_st_pill, UI_COL_CARD_HI);
    lv_style_set_bg_opa(&g_st_pill, LV_OPA_COVER);
    lv_style_set_border_color(&g_st_pill, UI_COL_BORDER);
    lv_style_set_border_width(&g_st_pill, 1);
    lv_style_set_radius(&g_st_pill, 100);
    lv_style_set_pad_hor(&g_st_pill, 10);
    lv_style_set_pad_ver(&g_st_pill, 4);

    lv_style_init(&g_st_btn_primary);
    lv_style_set_bg_color(&g_st_btn_primary, UI_COL_CYAN);
    lv_style_set_bg_opa(&g_st_btn_primary, LV_OPA_COVER);
    lv_style_set_text_color(&g_st_btn_primary, lv_color_hex(0x06101F));
    lv_style_set_radius(&g_st_btn_primary, 12);
    lv_style_set_border_width(&g_st_btn_primary, 0);

    lv_style_init(&g_st_btn_danger);
    lv_style_set_bg_color(&g_st_btn_danger, UI_COL_RED);
    lv_style_set_bg_opa(&g_st_btn_danger, LV_OPA_COVER);
    lv_style_set_text_color(&g_st_btn_danger, UI_COL_TEXT);
    lv_style_set_radius(&g_st_btn_danger, 12);
    lv_style_set_border_width(&g_st_btn_danger, 0);

    lv_style_init(&g_st_btn_ghost);
    lv_style_set_bg_color(&g_st_btn_ghost, UI_COL_CARD_HI);
    lv_style_set_bg_opa(&g_st_btn_ghost, LV_OPA_COVER);
    lv_style_set_border_color(&g_st_btn_ghost, UI_COL_BORDER);
    lv_style_set_border_width(&g_st_btn_ghost, 1);
    lv_style_set_text_color(&g_st_btn_ghost, UI_COL_TEXT);
    lv_style_set_radius(&g_st_btn_ghost, 12);

#if LV_USE_TINY_TTF
    if (xiaozhi_zh_ttf_size > 0U)
    {
        g_font_zh_full = lv_tiny_ttf_create_data(xiaozhi_zh_ttf, xiaozhi_zh_ttf_size, 20);
        if (g_font_zh_full != NULL)
        {
            g_font_zh_full->fallback = &lv_font_montserrat_20;
        }
        g_font_zh_small = lv_tiny_ttf_create_data(xiaozhi_zh_ttf, xiaozhi_zh_ttf_size, 16);
        if (g_font_zh_small != NULL)
        {
            g_font_zh_small->fallback = &lv_font_montserrat_16;
        }
    }
#endif

    g_theme_inited = true;
}

void ui_create_chrome(lv_obj_t *scr, const char *title,
                      ui_screen_id_t active_tab, ui_chrome_t *refs)
{
    static const struct { ui_screen_id_t page; const char *label; bool use_full_font; } tabs[] = {
        {UI_SCR_HOME,    LV_SYMBOL_HOME " 表盘", false},
        {UI_SCR_APPS,    LV_SYMBOL_LIST " 应用", false},
        {UI_SCR_XIAOZHI, "小智", true},
        {UI_SCR_FATIGUE, LV_SYMBOL_EYE_OPEN " 监测", false},
    };

    bool is_home = (active_tab == UI_SCR_HOME);

    lv_obj_add_style(scr, &g_st_screen, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* Top Status Bar (48px) */
    lv_obj_t *top = lv_obj_create(scr);
    lv_obj_set_size(top, UI_SCREEN_W, UI_TOPBAR_H);
    lv_obj_set_pos(top, 0, 0);
    lv_obj_set_style_bg_color(top, is_home ? lv_color_hex(0x0B1320) : lv_color_hex(0x0E1525), 0);
    lv_obj_set_style_bg_opa(top, is_home ? LV_OPA_30 : LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(top, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(top, is_home ? lv_color_hex(0x94B8D8) : UI_COL_BORDER, 0);
    lv_obj_set_style_border_opa(top, is_home ? LV_OPA_20 : LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(top, 1, 0);
    lv_obj_set_style_radius(top, 0, 0);
    lv_obj_set_style_pad_hor(top, 12, 0);
    lv_obj_set_style_pad_ver(top, 0, 0);
    lv_obj_remove_flag(top, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *lbl_title = lv_label_create(top);
    lv_label_set_text(lbl_title, title ? title : "");
    lv_obj_set_style_text_color(lbl_title, UI_COL_TEXT, 0);
    lv_obj_set_style_text_font(lbl_title,
                               (active_tab == UI_SCR_XIAOZHI ||
                                active_tab == UI_SCR_WIFI ||
                                active_tab == UI_SCR_WEATHER ||
                                active_tab == UI_SCR_SYSTEM ||
                                active_tab == UI_SCR_CLOCK_TOOLS ||
                                active_tab == UI_SCR_FATIGUE ||
                                active_tab == UI_SCR_MUSIC ||
                                active_tab == UI_SCR_GALLERY)
                                   ? ui_font_zh_full() : UI_FONT_ZH,
                               0);

    lv_obj_t *right_grp = lv_obj_create(top);
    lv_obj_set_size(right_grp, 210, 36);
    lv_obj_set_style_bg_opa(right_grp, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(right_grp, 0, 0);
    lv_obj_set_style_pad_all(right_grp, 0, 0);
    lv_obj_set_style_pad_column(right_grp, 8, 0);
    lv_obj_remove_flag(right_grp, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(right_grp, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right_grp, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *pill_core = lv_obj_create(right_grp);
    lv_obj_add_style(pill_core, &g_st_pill, 0);
    lv_obj_set_size(pill_core, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    if (is_home)
    {
        lv_obj_set_style_bg_color(pill_core, lv_color_hex(0x0B1320), 0);
        lv_obj_set_style_bg_opa(pill_core, LV_OPA_40, 0);
        lv_obj_set_style_border_color(pill_core, lv_color_hex(0x94B8D8), 0);
        lv_obj_set_style_border_opa(pill_core, LV_OPA_40, 0);
    }
    lv_obj_t *lbl_core = lv_label_create(pill_core);
    lv_label_set_text(lbl_core, "SOLO");
    lv_obj_set_style_text_font(lbl_core, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_core, UI_COL_MUTED, 0);

    lv_obj_t *lbl_wifi = lv_label_create(right_grp);
    lv_label_set_text(lbl_wifi, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_font(lbl_wifi, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_wifi, UI_COL_MUTED, 0);

    bool sys_active = (active_tab == UI_SCR_SYSTEM);
    lv_obj_t *pill_settings = lv_button_create(right_grp);
    lv_obj_add_style(pill_settings, &g_st_pill, 0);
    lv_obj_set_size(pill_settings, LV_SIZE_CONTENT, 28);
    lv_obj_set_style_pad_hor(pill_settings, 10, 0);
    lv_obj_set_style_pad_ver(pill_settings, 3, 0);
    lv_obj_set_style_shadow_width(pill_settings, 0, 0);
    if (sys_active)
    {
        lv_obj_set_style_bg_color(pill_settings, lv_color_hex(0x163459), 0);
        lv_obj_set_style_bg_opa(pill_settings, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(pill_settings, UI_COL_CYAN, 0);
    }
    else if (is_home)
    {
        lv_obj_set_style_bg_color(pill_settings, lv_color_hex(0x0B1320), 0);
        lv_obj_set_style_bg_opa(pill_settings, LV_OPA_40, 0);
        lv_obj_set_style_border_color(pill_settings, lv_color_hex(0x94B8D8), 0);
        lv_obj_set_style_border_opa(pill_settings, LV_OPA_40, 0);
    }
    else
    {
        lv_obj_set_style_bg_color(pill_settings, UI_COL_CARD_HI, 0);
        lv_obj_set_style_bg_opa(pill_settings, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(pill_settings, UI_COL_BORDER, 0);
    }
    lv_obj_add_event_cb(pill_settings, nav_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)UI_SCR_SYSTEM);
    lv_obj_t *lbl_settings = lv_label_create(pill_settings);
    lv_label_set_text(lbl_settings, LV_SYMBOL_SETTINGS " 设置");
    lv_obj_set_style_text_font(lbl_settings, ui_font_zh_small(), 0);
    lv_obj_set_style_text_color(lbl_settings,
                                sys_active ? UI_COL_CYAN : (is_home ? lv_color_hex(0xFFFFFF) : UI_COL_TEXT),
                                0);
    lv_obj_center(lbl_settings);

    /* Middle Body Region (688px) */
    lv_obj_t *body = lv_obj_create(scr);
    lv_obj_set_size(body, UI_SCREEN_W, UI_BODY_H);
    lv_obj_set_pos(body, 0, UI_TOPBAR_H);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_set_style_radius(body, 0, 0);
    lv_obj_set_style_pad_all(body, 12, 0);

    /* Bottom Navigation Dock (64px) */
    lv_obj_t *nav = lv_obj_create(scr);
    lv_obj_set_size(nav, UI_SCREEN_W, UI_NAVBAR_H);
    lv_obj_set_pos(nav, 0, UI_SCREEN_H - UI_NAVBAR_H);
    lv_obj_set_style_bg_color(nav, is_home ? lv_color_hex(0x0B1320) : lv_color_hex(0x0E1525), 0);
    lv_obj_set_style_bg_opa(nav, is_home ? LV_OPA_30 : LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(nav, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_color(nav, is_home ? lv_color_hex(0x94B8D8) : UI_COL_BORDER, 0);
    lv_obj_set_style_border_opa(nav, is_home ? LV_OPA_20 : LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(nav, 1, 0);
    lv_obj_set_style_radius(nav, 0, 0);
    lv_obj_set_style_pad_hor(nav, 8, 0);
    lv_obj_set_style_pad_ver(nav, 8, 0);
    lv_obj_set_style_pad_column(nav, 8, 0);
    lv_obj_remove_flag(nav, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(nav, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(nav, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    for (size_t i = 0; i < sizeof(tabs) / sizeof(tabs[0]); i++)
    {
        bool is_active = active_tab == tabs[i].page;
        lv_color_t active_bg = (tabs[i].page == UI_SCR_XIAOZHI) ? UI_COL_GEMINI_VIOLET : UI_COL_CYAN;
        lv_color_t active_fg = (tabs[i].page == UI_SCR_XIAOZHI) ? UI_COL_TEXT : lv_color_hex(0x06101F);
        lv_color_t inactive_fg = is_home ? lv_color_hex(0xE2ECF7) : UI_COL_MUTED;
        lv_obj_t *btn = lv_button_create(nav);
        lv_obj_set_flex_grow(btn, 1);
        lv_obj_set_height(btn, 46);
        lv_obj_set_style_radius(btn, 12, 0);
        if (is_active)
        {
            lv_obj_set_style_bg_color(btn, active_bg, 0);
            lv_obj_set_style_bg_opa(btn, is_home ? LV_OPA_70 : LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(btn, 0, 0);
        }
        else if (is_home)
        {
            lv_obj_set_style_bg_color(btn, lv_color_hex(0x0B1320), 0);
            lv_obj_set_style_bg_opa(btn, LV_OPA_30, 0);
            lv_obj_set_style_border_width(btn, 1, 0);
            lv_obj_set_style_border_color(btn, lv_color_hex(0x94B8D8), 0);
            lv_obj_set_style_border_opa(btn, LV_OPA_30, 0);
        }
        else
        {
            lv_obj_set_style_bg_color(btn, UI_COL_CARD, 0);
            lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(btn, 1, 0);
            lv_obj_set_style_border_color(btn, UI_COL_BORDER, 0);
        }
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_add_event_cb(btn, nav_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)tabs[i].page);

        if (tabs[i].page == UI_SCR_XIAOZHI)
        {
            lv_obj_t *row = lv_obj_create(btn);
            lv_obj_remove_style_all(row);
            lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_column(row, 4, 0);
            lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_center(row);

            lv_obj_t *ico = lv_image_create(row);
            lv_image_set_src(ico, &g_ui_gemini_icon_32);
            lv_obj_set_size(ico, 22, 22);
            lv_image_set_scale(ico, 176); /* 32 * 176 / 256 = 22 px */
            lv_obj_remove_flag(ico, LV_OBJ_FLAG_CLICKABLE);

            lv_obj_t *blbl = lv_label_create(row);
            lv_label_set_text(blbl, tabs[i].label);
            lv_obj_set_style_text_font(blbl, ui_font_zh_full(), 0);
            lv_obj_set_style_text_color(blbl, is_active ? active_fg : inactive_fg, 0);
        }
        else
        {
            lv_obj_t *blbl = lv_label_create(btn);
            lv_label_set_text(blbl, tabs[i].label);
            lv_obj_set_style_text_font(blbl, tabs[i].use_full_font ? ui_font_zh_full() : UI_FONT_ZH, 0);
            lv_obj_set_style_text_color(blbl, is_active ? active_fg : inactive_fg, 0);
            lv_obj_center(blbl);
        }
    }

    if (refs != NULL)
    {
        refs->topbar     = top;
        refs->lbl_title  = lbl_title;
        refs->pill_core  = pill_core;
        refs->lbl_core   = lbl_core;
        refs->lbl_wifi   = lbl_wifi;
        refs->pill_alert = pill_settings;
        refs->lbl_alert  = NULL;
        refs->body       = body;
        refs->navbar     = nav;
    }
}

void ui_update_chrome(ui_chrome_t *refs)
{
    rt_bool_t dc_on = RT_FALSE;
    uint32_t hb = 0, rtt = 0;
    wifi_status_t wst;

    if (refs == NULL) return;

    dualcore_get_brief(&dc_on, &hb, &rtt);
    wifi_mgr_get_status(&wst);

    ui_set_label_text(refs->lbl_core, dc_on ? "DUAL" : "SOLO");
    ui_set_text_color(refs->lbl_core, dc_on ? UI_COL_GREEN : UI_COL_AMBER);
    ui_set_text_color(refs->lbl_wifi, wst.ready ? UI_COL_CYAN : (wst.connected ? UI_COL_AMBER : UI_COL_MUTED));
}
