#include <rtthread.h>
#include <stdio.h>
#include "ui_theme.h"
#include "ui_manager.h"
#include "dualcore_bridge.h"
#include "alert_manager.h"

#define SYS_PULLDOWN_OPTS 8U

static const struct
{
    ui_screen_id_t id;
    const char    *label;
} g_pd_opts[SYS_PULLDOWN_OPTS] = {
    {UI_SCR_GALLERY,     "图库"},
    {UI_SCR_WEATHER,     "天气"},
    {UI_SCR_XIAOZHI,     "小智"},
    {UI_SCR_FATIGUE,     "监测"},
    {UI_SCR_CLOCK_TOOLS, "日历计时"},
    {UI_SCR_BRIGHTNESS,  "亮度"},
    {UI_SCR_WIFI,        "无线网络"},
    {UI_SCR_SYSTEM,      "系统"},
};

static ui_chrome_t g_chrome;
static lv_obj_t *g_lbl_m55;
static lv_obj_t *g_lbl_m33;
static lv_obj_t *g_lbl_ipc;
static lv_obj_t *g_lbl_vol;
static lv_obj_t *g_lbl_sd;
static lv_obj_t *g_lbl_pd_cur;
static lv_obj_t *g_pd_btns[SYS_PULLDOWN_OPTS];
static lv_obj_t *g_pd_lbls[SYS_PULLDOWN_OPTS];

static void sys_ping_cb(lv_event_t *e)      { (void)e; (void)dualcore_send_ping(); }
static void sys_test_snd_cb(lv_event_t *e)  { (void)e; (void)alert_test_sound(); }
static void sys_stop_snd_cb(lv_event_t *e)  { (void)e; alert_stop(); }

static void sys_vol_sl_cb(lv_event_t *e)
{
    int v = lv_slider_get_value(lv_event_get_target(e));
    alert_set_volume(v);
    lv_label_set_text_fmt(g_lbl_vol, "Alarm Volume: %d", v);
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

lv_obj_t *ui_screen_system_create(void)
{
    const lv_font_t *f_small = ui_font_zh_small();

    lv_obj_t *scr = lv_obj_create(NULL);
    ui_create_chrome(scr, "系统设置 · 双核", UI_SCR_SYSTEM, &g_chrome);

    lv_obj_t *body = g_chrome.body;
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(body, 10, 0);
    lv_obj_set_style_pad_row(body, 8, 0);

    /* 1. Watch Face Pull-Down & Swipe Gesture Settings Card (460 x 140) */
    lv_obj_t *pd_card = lv_obj_create(body);
    lv_obj_add_style(pd_card, &g_st_card, 0);
    lv_obj_set_size(pd_card, 460, 140);
    lv_obj_set_style_pad_hor(pd_card, 12, 0);
    lv_obj_set_style_pad_ver(pd_card, 8, 0);
    lv_obj_remove_flag(pd_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(pd_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(pd_card, 6, 0);

    lv_obj_t *pd_hdr = lv_obj_create(pd_card);
    lv_obj_remove_style_all(pd_hdr);
    lv_obj_set_size(pd_hdr, 434, 24);
    lv_obj_remove_flag(pd_hdr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(pd_hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(pd_hdr, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *pd_t = lv_label_create(pd_hdr);
    lv_label_set_text(pd_t, "表盘下拉快捷页面 (支持左右滑切页)");
    lv_obj_set_style_text_font(pd_t, f_small, 0);
    lv_obj_set_style_text_color(pd_t, UI_COL_CYAN, 0);

    g_lbl_pd_cur = lv_label_create(pd_hdr);
    lv_label_set_text(g_lbl_pd_cur, "当前: 图库");
    lv_obj_set_style_text_font(g_lbl_pd_cur, f_small, 0);
    lv_obj_set_style_text_color(g_lbl_pd_cur, UI_COL_GREEN, 0);

    lv_obj_t *pd_grid = lv_obj_create(pd_card);
    lv_obj_remove_style_all(pd_grid);
    lv_obj_set_size(pd_grid, 434, 86);
    lv_obj_remove_flag(pd_grid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(pd_grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(pd_grid, 6, 0);
    lv_obj_set_style_pad_column(pd_grid, 6, 0);

    for (unsigned i = 0; i < SYS_PULLDOWN_OPTS; i++)
    {
        g_pd_btns[i] = lv_button_create(pd_grid);
        lv_obj_add_style(g_pd_btns[i], &g_st_btn_ghost, 0);
        lv_obj_set_size(g_pd_btns[i], 104, 36);
        lv_obj_set_style_radius(g_pd_btns[i], 10, 0);
        lv_obj_add_event_cb(g_pd_btns[i], sys_pulldown_btn_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)g_pd_opts[i].id);

        g_pd_lbls[i] = lv_label_create(g_pd_btns[i]);
        lv_label_set_text(g_pd_lbls[i], g_pd_opts[i].label);
        lv_obj_set_style_text_font(g_pd_lbls[i], f_small, 0);
        lv_obj_center(g_pd_lbls[i]);
    }
    sys_refresh_pulldown_pills();

    /* 2. Dual-Core Processor Card (460 x 146) */
    lv_obj_t *dc_card = lv_obj_create(body);
    lv_obj_add_style(dc_card, &g_st_card, 0);
    lv_obj_set_size(dc_card, 460, 146);
    lv_obj_set_style_pad_hor(dc_card, 12, 0);
    lv_obj_set_style_pad_ver(dc_card, 8, 0);
    lv_obj_remove_flag(dc_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(dc_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(dc_card, 5, 0);

    lv_obj_t *hdr = lv_obj_create(dc_card);
    lv_obj_remove_style_all(hdr);
    lv_obj_set_size(hdr, 434, 30);
    lv_obj_remove_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *ht = lv_label_create(hdr);
    lv_label_set_text(ht, "DUAL-CORE ARCHITECTURE (IPC1)");
    lv_obj_set_style_text_font(ht, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(ht, UI_COL_CYAN, 0);

    lv_obj_t *bping = lv_button_create(hdr);
    lv_obj_add_style(bping, &g_st_btn_primary, 0);
    lv_obj_set_size(bping, 100, 28);
    lv_obj_add_event_cb(bping, sys_ping_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lp = lv_label_create(bping);
    lv_label_set_text(lp, "Ping CM33");
    lv_obj_set_style_text_font(lp, &lv_font_montserrat_12, 0);
    lv_obj_center(lp);

    g_lbl_m55 = lv_label_create(dc_card);
    lv_label_set_text(g_lbl_m55, "CM55 @ 400MHz | Uptime: 0s | Console: UART2");
    lv_obj_set_style_text_font(g_lbl_m55, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(g_lbl_m55, UI_COL_TEXT, 0);

    g_lbl_m33 = lv_label_create(dc_card);
    lv_label_set_text(g_lbl_m33, "CM33 @ 200MHz | State: Checking...");
    lv_obj_set_style_text_font(g_lbl_m33, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(g_lbl_m33, UI_COL_AMBER, 0);

    g_lbl_ipc = lv_label_create(dc_card);
    lv_label_set_text(g_lbl_ipc, "IPC1 Stats: TX=0 RX=0 RTT=0ms");
    lv_obj_set_style_text_font(g_lbl_ipc, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(g_lbl_ipc, UI_COL_MUTED, 0);

    /* 3. Display & Audio Controls Card (460 x 198) */
    lv_obj_t *dacard = lv_obj_create(body);
    lv_obj_add_style(dacard, &g_st_card, 0);
    lv_obj_set_size(dacard, 460, 198);
    lv_obj_set_style_pad_hor(dacard, 12, 0);
    lv_obj_set_style_pad_ver(dacard, 8, 0);
    lv_obj_remove_flag(dacard, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(dacard, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(dacard, 8, 0);

    lv_obj_t *da_hdr = lv_label_create(dacard);
    lv_label_set_text(da_hdr, "DISPLAY & AUDIO CONTROL");
    lv_obj_set_style_text_font(da_hdr, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(da_hdr, UI_COL_CYAN, 0);

    lv_obj_t *bl_label = lv_label_create(dacard);
    lv_obj_set_style_text_font(bl_label, UI_FONT_ZH, 0);
    lv_obj_set_style_text_color(bl_label, UI_COL_TEXT, 0);

    lv_obj_t *sl_bl = ui_brightness_slider_create(dacard, bl_label);
    lv_obj_set_size(sl_bl, 430, 12);

    g_lbl_vol = lv_label_create(dacard);
    lv_label_set_text_fmt(g_lbl_vol, "Alarm Volume: %d", alert_get_volume());
    lv_obj_set_style_text_font(g_lbl_vol, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(g_lbl_vol, UI_COL_TEXT, 0);

    lv_obj_t *sl_v = lv_slider_create(dacard);
    lv_obj_set_size(sl_v, 430, 12);
    lv_slider_set_range(sl_v, 0, 99);
    lv_slider_set_value(sl_v, alert_get_volume(), LV_ANIM_OFF);
    lv_obj_set_style_bg_color(sl_v, lv_color_hex(0x1E293B), LV_PART_MAIN);
    lv_obj_set_style_bg_color(sl_v, UI_COL_GREEN, LV_PART_INDICATOR);
    lv_obj_add_event_cb(sl_v, sys_vol_sl_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *abtns = lv_obj_create(dacard);
    lv_obj_remove_style_all(abtns);
    lv_obj_set_size(abtns, 430, 38);
    lv_obj_set_style_pad_column(abtns, 10, 0);
    lv_obj_remove_flag(abtns, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(abtns, LV_FLEX_FLOW_ROW);

    lv_obj_t *btst = lv_button_create(abtns);
    lv_obj_add_style(btst, &g_st_btn_ghost, 0);
    lv_obj_set_flex_grow(btst, 1);
    lv_obj_set_height(btst, 36);
    lv_obj_add_event_cb(btst, sys_test_snd_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lt = lv_label_create(btst);
    lv_label_set_text(lt, LV_SYMBOL_AUDIO " Test Alarm");
    lv_obj_center(lt);

    lv_obj_t *bstp = lv_button_create(abtns);
    lv_obj_add_style(bstp, &g_st_btn_danger, 0);
    lv_obj_set_flex_grow(bstp, 1);
    lv_obj_set_height(bstp, 36);
    lv_obj_add_event_cb(bstp, sys_stop_snd_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ls = lv_label_create(bstp);
    lv_label_set_text(ls, LV_SYMBOL_MUTE " Stop Audio");
    lv_obj_center(ls);

    /* 4. Hardware Topology & Storage Card (460 x 154) */
    lv_obj_t *hwcard = lv_obj_create(body);
    lv_obj_add_style(hwcard, &g_st_card, 0);
    lv_obj_set_size(hwcard, 460, 154);
    lv_obj_set_style_pad_hor(hwcard, 12, 0);
    lv_obj_set_style_pad_ver(hwcard, 8, 0);
    lv_obj_remove_flag(hwcard, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(hwcard, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(hwcard, 5, 0);

    lv_obj_t *hhdr = lv_label_create(hwcard);
    lv_label_set_text(hhdr, "HARDWARE & STORAGE");
    lv_obj_set_style_text_font(hhdr, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(hhdr, UI_COL_CYAN, 0);

    g_lbl_sd = lv_label_create(hwcard);
    lv_label_set_text(g_lbl_sd, "Recorded SD Alerts: 0");
    lv_obj_set_style_text_font(g_lbl_sd, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(g_lbl_sd, UI_COL_TEXT, 0);

    lv_obj_t *hw_info = lv_label_create(hwcard);
    lv_label_set_text(hw_info,
                      "• I2C1: AHT20  |  I2C2: ST7102 Touch\n"
                      "• SDHC0: CYW55500 Wi-Fi 6  |  SDHC1: /sdcard\n"
                      "• Audio: ES8388 I2S + PDM Mic  |  Ethos-U55 + VG-Lite");
    lv_obj_set_style_text_font(hw_info, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(hw_info, UI_COL_MUTED, 0);
    lv_obj_set_style_text_line_space(hw_info, 4, 0);

    ui_screen_system_update();
    return scr;
}

void ui_screen_system_update(void)
{
    dualcore_status_t dc;
    uint32_t up_s = (uint32_t)(rt_tick_get() / RT_TICK_PER_SECOND);

    ui_update_chrome(&g_chrome);
    sys_refresh_pulldown_pills();
    dualcore_get_status(&dc);

    lv_label_set_text_fmt(g_lbl_m55, "CM55 @ 400MHz | Uptime: %lus | Console: UART2",
                          (unsigned long)up_s);

    if (dc.m33_online)
    {
        lv_label_set_text_fmt(g_lbl_m33, "CM33 @ 200MHz | ONLINE | HB #%lu (%lus)",
                              (unsigned long)dc.m33_hb_seq, (unsigned long)dc.m33_uptime_s);
        lv_obj_set_style_text_color(g_lbl_m33, UI_COL_GREEN, 0);
    }
    else
    {
        lv_label_set_text(g_lbl_m33, "CM33 @ 200MHz | OFFLINE (M55 SOLO Fallback)");
        lv_obj_set_style_text_color(g_lbl_m33, UI_COL_AMBER, 0);
    }

    lv_label_set_text_fmt(g_lbl_ipc, "IPC1 (%s) | TX=%lu RX=%lu | RTT=%lu ms",
                          dc.ipc_ready ? "ipc0/ipc1" : "unavail",
                          (unsigned long)dc.tx_count, (unsigned long)dc.rx_count,
                          (unsigned long)dc.last_rtt_ms);

    lv_label_set_text_fmt(g_lbl_sd, "Recorded SD Alerts: %lu  |  Threshold: %.0f%%",
                          (unsigned long)alert_get_danger_count(),
                          (double)(alert_get_threshold() * 100.0f));
}
