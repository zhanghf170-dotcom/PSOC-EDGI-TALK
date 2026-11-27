#include <rtthread.h>
#include <stdio.h>
#include "ui_theme.h"
#include "ui_manager.h"
#include "dualcore_bridge.h"
#include "alert_manager.h"
#include "music_player.h"
#include "sd_assets.h"

#define SYS_PULLDOWN_OPTS 8U

static const struct
{
    ui_screen_id_t id;
    const char    *label;
} g_pd_opts[SYS_PULLDOWN_OPTS] = {
    {UI_SCR_GALLERY,    "图库"},
    {UI_SCR_MUSIC,      "音乐"},
    {UI_SCR_WEATHER,    "天气"},
    {UI_SCR_XIAOZHI,    "小智"},
    {UI_SCR_FATIGUE,    "监测"},
    {UI_SCR_BRIGHTNESS, "亮度"},
    {UI_SCR_WIFI,       "无线网络"},
    {UI_SCR_SYSTEM,     "设置"},
};

static ui_chrome_t g_chrome;
static lv_obj_t *g_vol_icon;
static lv_obj_t *g_lbl_vol;
static lv_obj_t *g_sl_vol;
static lv_obj_t *g_lbl_pd_cur;
static lv_obj_t *g_pd_btns[SYS_PULLDOWN_OPTS];
static lv_obj_t *g_pd_lbls[SYS_PULLDOWN_OPTS];
static lv_obj_t *g_lbl_sd;
static lv_obj_t *g_lbl_dc;

static void sys_sync_volume_ui(bool update_slider)
{
    int vol = alert_get_volume();
    if (g_lbl_vol != NULL)
    {
        char buf[32];
        rt_snprintf(buf, sizeof(buf), "全局音量  %d%%", vol);
        ui_set_label_text(g_lbl_vol, buf);
    }
    if (g_vol_icon != NULL)
    {
        ui_set_label_text(g_vol_icon, (vol == 0) ? LV_SYMBOL_MUTE : LV_SYMBOL_VOLUME_MAX);
        lv_obj_set_style_text_color(g_vol_icon,
                                    (vol == 0) ? UI_COL_MUTED : (vol >= 80 ? UI_COL_AMBER : UI_COL_CYAN),
                                    0);
    }
    if (update_slider && g_sl_vol != NULL && !lv_obj_has_state(g_sl_vol, LV_STATE_PRESSED))
    {
        lv_slider_set_value(g_sl_vol, vol, LV_ANIM_OFF);
    }
}

static void sys_vol_sl_cb(lv_event_t *e)
{
    int v = lv_slider_get_value(lv_event_get_target(e));
    alert_set_volume(v);
    sys_sync_volume_ui(false);
}

static void sys_vol_released_cb(lv_event_t *e)
{
    (void)e;
    alert_save_volume_cfg();
}

static void sys_vol_preset_cb(lv_event_t *e)
{
    int val = (int)(intptr_t)lv_event_get_user_data(e);
    alert_set_volume(val);
    alert_save_volume_cfg();
    sys_sync_volume_ui(true);
}

static void sys_vol_step_cb(lv_event_t *e)
{
    int delta = (int)(intptr_t)lv_event_get_user_data(e);
    int v = alert_get_volume() + delta;
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    alert_set_volume(v);
    alert_save_volume_cfg();
    sys_sync_volume_ui(true);
}

static void sys_test_snd_cb(lv_event_t *e)
{
    (void)e;
    (void)alert_test_sound();
}

static void sys_stop_snd_cb(lv_event_t *e)
{
    (void)e;
    alert_stop();
    (void)music_player_stop();
}

static void sys_refresh_pulldown_pills(void)
{
    ui_screen_id_t cur = ui_get_pulldown_target();
    if (g_lbl_pd_cur != NULL)
    {
        char buf[48];
        rt_snprintf(buf, sizeof(buf), "当前: %s", ui_pulldown_target_name(cur));
        ui_set_label_text(g_lbl_pd_cur, buf);
    }
    for (unsigned i = 0; i < SYS_PULLDOWN_OPTS; i++)
    {
        if (g_pd_btns[i] == NULL || g_pd_lbls[i] == NULL) continue;
        bool active = (g_pd_opts[i].id == cur);
        lv_obj_set_style_bg_color(g_pd_btns[i], active ? lv_color_hex(0x163459) : UI_COL_CARD_HI, 0);
        lv_obj_set_style_border_color(g_pd_btns[i], active ? UI_COL_CYAN : UI_COL_BORDER, 0);
        lv_obj_set_style_text_color(g_pd_lbls[i], active ? UI_COL_CYAN : UI_COL_TEXT, 0);
    }
}

static void sys_pulldown_btn_cb(lv_event_t *e)
{
    ui_screen_id_t id = (ui_screen_id_t)(intptr_t)lv_event_get_user_data(e);
    ui_set_pulldown_target(id);
    sys_refresh_pulldown_pills();
}

static lv_obj_t *make_btn_lbl(lv_obj_t *btn, const char *txt, const lv_font_t *font, lv_color_t col)
{
    lv_obj_t *l = lv_label_create(btn);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, col, 0);
    lv_obj_center(l);
    return l;
}

lv_obj_t *ui_screen_system_create(void)
{
    const lv_font_t *f_full  = ui_font_zh_full();
    const lv_font_t *f_small = ui_font_zh_small();

    lv_obj_t *scr = lv_obj_create(NULL);
    ui_create_chrome(scr, "设置 · 音量调节", UI_SCR_SYSTEM, &g_chrome);

    lv_obj_t *body = g_chrome.body;
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(body, 10, 0);
    lv_obj_set_style_pad_row(body, 10, 0);

    /* 1. Primary Global Volume Control Card (460 x 380) */
    lv_obj_t *vcard = lv_obj_create(body);
    lv_obj_add_style(vcard, &g_st_card, 0);
    lv_obj_set_size(vcard, 460, 380);
    lv_obj_set_style_pad_hor(vcard, 16, 0);
    lv_obj_set_style_pad_ver(vcard, 16, 0);
    lv_obj_remove_flag(vcard, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(vcard, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(vcard, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *vhdr = lv_label_create(vcard);
    lv_label_set_text(vhdr, "系统音频输出音量 (音乐 · 警报 · 小智)");
    lv_obj_set_style_text_font(vhdr, f_small, 0);
    lv_obj_set_style_text_color(vhdr, UI_COL_MUTED, 0);

    g_vol_icon = lv_label_create(vcard);
    lv_label_set_text(g_vol_icon, LV_SYMBOL_VOLUME_MAX);
    lv_obj_set_style_text_font(g_vol_icon, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(g_vol_icon, UI_COL_CYAN, 0);

    g_lbl_vol = lv_label_create(vcard);
    lv_obj_set_style_text_font(g_lbl_vol, f_full, 0);
    lv_obj_set_style_text_color(g_lbl_vol, UI_COL_TEXT, 0);
    lv_label_set_text(g_lbl_vol, "全局音量  75%");

    lv_obj_t *sl_box = lv_obj_create(vcard);
    lv_obj_remove_style_all(sl_box);
    lv_obj_set_size(sl_box, 424, 48);
    lv_obj_remove_flag(sl_box, LV_OBJ_FLAG_SCROLLABLE);

    g_sl_vol = lv_slider_create(sl_box);
    lv_obj_set_size(g_sl_vol, 392, 22);
    lv_obj_align(g_sl_vol, LV_ALIGN_TOP_MID, 0, 4);
    lv_obj_set_ext_click_area(g_sl_vol, 18);
    lv_slider_set_range(g_sl_vol, 0, 100);
    lv_slider_set_value(g_sl_vol, alert_get_volume(), LV_ANIM_OFF);
    lv_obj_set_style_bg_color(g_sl_vol, lv_color_hex(0x1E293B), LV_PART_MAIN);
    lv_obj_set_style_bg_color(g_sl_vol, UI_COL_CYAN, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(g_sl_vol, UI_COL_TEXT, LV_PART_KNOB);
    lv_obj_add_event_cb(g_sl_vol, sys_vol_sl_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(g_sl_vol, sys_vol_released_cb, LV_EVENT_RELEASED, NULL);

    lv_obj_t *l_min = lv_label_create(sl_box);
    lv_label_set_text(l_min, "0%");
    lv_obj_set_style_text_font(l_min, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(l_min, UI_COL_MUTED, 0);
    lv_obj_align(l_min, LV_ALIGN_BOTTOM_LEFT, 16, 0);

    lv_obj_t *l_max = lv_label_create(sl_box);
    lv_label_set_text(l_max, "100%");
    lv_obj_set_style_text_font(l_max, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(l_max, UI_COL_MUTED, 0);
    lv_obj_align(l_max, LV_ALIGN_BOTTOM_RIGHT, -16, 0);

    /* Quick Volume Step & Presets Row */
    lv_obj_t *qrow = lv_obj_create(vcard);
    lv_obj_remove_style_all(qrow);
    lv_obj_set_size(qrow, 424, 46);
    lv_obj_remove_flag(qrow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(qrow, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(qrow, 8, 0);

    static const struct
    {
        const char   *txt;
        lv_event_cb_t cb;
        intptr_t      arg;
    } qbtns[5] = {
        {"静音", sys_vol_preset_cb, 0},
        {"-10%", sys_vol_step_cb,  -10},
        {"50%",  sys_vol_preset_cb, 50},
        {"+10%", sys_vol_step_cb,   10},
        {"最大", sys_vol_preset_cb, 100},
    };
    for (unsigned i = 0; i < 5U; i++)
    {
        lv_obj_t *b = lv_button_create(qrow);
        lv_obj_add_style(b, &g_st_btn_ghost, 0);
        lv_obj_set_flex_grow(b, 1);
        lv_obj_set_height(b, 44);
        lv_obj_set_style_pad_hor(b, 4, 0);
        lv_obj_set_style_pad_ver(b, 4, 0);
        lv_obj_add_event_cb(b, qbtns[i].cb, LV_EVENT_CLICKED, (void *)qbtns[i].arg);
        (void)make_btn_lbl(b, qbtns[i].txt, f_small, UI_COL_TEXT);
    }

    /* Audio Test & Stop Row */
    lv_obj_t *abtns = lv_obj_create(vcard);
    lv_obj_remove_style_all(abtns);
    lv_obj_set_size(abtns, 424, 48);
    lv_obj_set_style_pad_column(abtns, 10, 0);
    lv_obj_remove_flag(abtns, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(abtns, LV_FLEX_FLOW_ROW);

    lv_obj_t *btst = lv_button_create(abtns);
    lv_obj_add_style(btst, &g_st_btn_primary, 0);
    lv_obj_set_flex_grow(btst, 1);
    lv_obj_set_height(btst, 46);
    lv_obj_add_event_cb(btst, sys_test_snd_cb, LV_EVENT_CLICKED, NULL);
    (void)make_btn_lbl(btst, LV_SYMBOL_AUDIO " 试听音效", f_full, lv_color_hex(0x06101F));

    lv_obj_t *bstp = lv_button_create(abtns);
    lv_obj_add_style(bstp, &g_st_btn_danger, 0);
    lv_obj_set_flex_grow(bstp, 1);
    lv_obj_set_height(bstp, 46);
    lv_obj_add_event_cb(bstp, sys_stop_snd_cb, LV_EVENT_CLICKED, NULL);
    (void)make_btn_lbl(bstp, LV_SYMBOL_MUTE " 停止音频", f_full, UI_COL_TEXT);

    /* 2. Pull-down Shortcut & Storage/Core Summary Card (460 x 268) */
    lv_obj_t *scard = lv_obj_create(body);
    lv_obj_add_style(scard, &g_st_card, 0);
    lv_obj_set_size(scard, 460, 268);
    lv_obj_set_style_pad_hor(scard, 12, 0);
    lv_obj_set_style_pad_ver(scard, 10, 0);
    lv_obj_remove_flag(scard, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(scard, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(scard, 8, 0);

    lv_obj_t *pd_hdr = lv_obj_create(scard);
    lv_obj_remove_style_all(pd_hdr);
    lv_obj_set_size(pd_hdr, 434, 24);
    lv_obj_remove_flag(pd_hdr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(pd_hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(pd_hdr, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *pd_t = lv_label_create(pd_hdr);
    lv_label_set_text(pd_t, "表盘下拉快捷页面");
    lv_obj_set_style_text_font(pd_t, f_small, 0);
    lv_obj_set_style_text_color(pd_t, UI_COL_CYAN, 0);

    g_lbl_pd_cur = lv_label_create(pd_hdr);
    lv_label_set_text(g_lbl_pd_cur, "当前: 图库");
    lv_obj_set_style_text_font(g_lbl_pd_cur, f_small, 0);
    lv_obj_set_style_text_color(g_lbl_pd_cur, UI_COL_GREEN, 0);

    lv_obj_t *pd_grid = lv_obj_create(scard);
    lv_obj_remove_style_all(pd_grid);
    lv_obj_set_size(pd_grid, 434, 92);
    lv_obj_remove_flag(pd_grid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(pd_grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(pd_grid, 8, 0);
    lv_obj_set_style_pad_column(pd_grid, 6, 0);

    for (unsigned i = 0; i < SYS_PULLDOWN_OPTS; i++)
    {
        g_pd_btns[i] = lv_button_create(pd_grid);
        lv_obj_add_style(g_pd_btns[i], &g_st_btn_ghost, 0);
        lv_obj_set_size(g_pd_btns[i], 104, 38);
        lv_obj_set_style_pad_hor(g_pd_btns[i], 6, 0);
        lv_obj_set_style_pad_ver(g_pd_btns[i], 4, 0);
        lv_obj_set_style_radius(g_pd_btns[i], 10, 0);
        lv_obj_add_event_cb(g_pd_btns[i], sys_pulldown_btn_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)g_pd_opts[i].id);

        g_pd_lbls[i] = lv_label_create(g_pd_btns[i]);
        lv_label_set_text(g_pd_lbls[i], g_pd_opts[i].label);
        lv_obj_set_style_text_font(g_pd_lbls[i], f_small, 0);
        lv_obj_center(g_pd_lbls[i]);
    }
    sys_refresh_pulldown_pills();

    g_lbl_sd = lv_label_create(scard);
    lv_label_set_text(g_lbl_sd, "TF 卡: 检测中...");
    lv_obj_set_style_text_font(g_lbl_sd, f_small, 0);
    lv_obj_set_style_text_color(g_lbl_sd, UI_COL_TEXT, 0);

    g_lbl_dc = lv_label_create(scard);
    lv_label_set_text(g_lbl_dc, "双核状态: CM55 400MHz");
    lv_obj_set_style_text_font(g_lbl_dc, f_small, 0);
    lv_obj_set_style_text_color(g_lbl_dc, UI_COL_MUTED, 0);

    ui_screen_system_update();
    return scr;
}

void ui_screen_system_update(void)
{
    dualcore_status_t dc;
    char buf[96];

    ui_update_chrome(&g_chrome);
    sys_sync_volume_ui(true);
    sys_refresh_pulldown_pills();
    dualcore_get_status(&dc);

    rt_snprintf(buf, sizeof(buf), "TF 卡: %s  |  曲目: %lu  |  告警记录: %lu",
                sdcard_is_mounted() ? "已挂载 (/sdcard)" : "未插入",
                (unsigned long)music_player_get_track_count(),
                (unsigned long)alert_get_danger_count());
    ui_set_label_text(g_lbl_sd, buf);

    if (dc.m33_online)
    {
        rt_snprintf(buf, sizeof(buf), "双核: CM55 400MHz + CM33 ONLINE (RTT %lums)",
                    (unsigned long)dc.last_rtt_ms);
        lv_obj_set_style_text_color(g_lbl_dc, UI_COL_GREEN, 0);
    }
    else
    {
        rt_snprintf(buf, sizeof(buf), "双核: CM55 400MHz (M55 SOLO)");
        lv_obj_set_style_text_color(g_lbl_dc, UI_COL_AMBER, 0);
    }
    ui_set_label_text(g_lbl_dc, buf);
}
