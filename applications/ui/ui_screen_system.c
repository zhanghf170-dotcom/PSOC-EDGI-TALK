#include <rtthread.h>
#include <stdio.h>
#include "ui_theme.h"
#include "ui_manager.h"
#include "dualcore_bridge.h"
#include "alert_manager.h"

static ui_chrome_t g_chrome;
static lv_obj_t *g_lbl_m55;
static lv_obj_t *g_lbl_m33;
static lv_obj_t *g_lbl_ipc;
static lv_obj_t *g_lbl_vol;
static lv_obj_t *g_lbl_sd;

static void sys_ping_cb(lv_event_t *e)      { (void)e; (void)dualcore_send_ping(); }
static void sys_test_snd_cb(lv_event_t *e)  { (void)e; (void)alert_test_sound(); }
static void sys_stop_snd_cb(lv_event_t *e)  { (void)e; alert_stop(); }

static void sys_vol_sl_cb(lv_event_t *e)
{
    int v = lv_slider_get_value(lv_event_get_target(e));
    alert_set_volume(v);
    lv_label_set_text_fmt(g_lbl_vol, "Alarm Volume: %d", v);
}

lv_obj_t *ui_screen_system_create(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    ui_create_chrome(scr, "SYSTEM & DUAL-CORE", UI_SCR_SYSTEM, &g_chrome);

    lv_obj_t *body = g_chrome.body;
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(body, 12, 0);

    /* 1. Dual-Core Processor Card */
    lv_obj_t *dc_card = lv_obj_create(body);
    lv_obj_add_style(dc_card, &g_st_card, 0);
    lv_obj_set_size(dc_card, 456, 212);
    lv_obj_remove_flag(dc_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(dc_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(dc_card, 8, 0);

    lv_obj_t *hdr = lv_obj_create(dc_card);
    lv_obj_set_size(hdr, 430, 36);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(hdr, 0, 0);
    lv_obj_set_style_pad_all(hdr, 0, 0);
    lv_obj_remove_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *ht = lv_label_create(hdr);
    lv_label_set_text(ht, "DUAL-CORE ARCHITECTURE (IPC1)");
    lv_obj_set_style_text_font(ht, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(ht, UI_COL_CYAN, 0);

    lv_obj_t *bping = lv_button_create(hdr);
    lv_obj_add_style(bping, &g_st_btn_primary, 0);
    lv_obj_set_size(bping, 108, 32);
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

    /* 2. Display & Audio Controls Card */
    lv_obj_t *dacard = lv_obj_create(body);
    lv_obj_add_style(dacard, &g_st_card, 0);
    lv_obj_set_size(dacard, 456, 212);
    lv_obj_remove_flag(dacard, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(dacard, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(dacard, 10, 0);

    lv_obj_t *da_hdr = lv_label_create(dacard);
    lv_label_set_text(da_hdr, "DISPLAY & AUDIO CONTROL");
    lv_obj_set_style_text_font(da_hdr, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(da_hdr, UI_COL_CYAN, 0);

    lv_obj_t *bl_label = lv_label_create(dacard);
    lv_obj_set_style_text_font(bl_label, UI_FONT_ZH, 0);
    lv_obj_set_style_text_color(bl_label, UI_COL_TEXT, 0);

    lv_obj_t *sl_bl = ui_brightness_slider_create(dacard, bl_label);
    lv_obj_set_size(sl_bl, 426, 12);

    g_lbl_vol = lv_label_create(dacard);
    lv_label_set_text_fmt(g_lbl_vol, "Alarm Volume: %d", alert_get_volume());
    lv_obj_set_style_text_font(g_lbl_vol, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(g_lbl_vol, UI_COL_TEXT, 0);

    lv_obj_t *sl_v = lv_slider_create(dacard);
    lv_obj_set_size(sl_v, 426, 12);
    lv_slider_set_range(sl_v, 0, 99);
    lv_slider_set_value(sl_v, alert_get_volume(), LV_ANIM_OFF);
    lv_obj_set_style_bg_color(sl_v, lv_color_hex(0x1E293B), LV_PART_MAIN);
    lv_obj_set_style_bg_color(sl_v, UI_COL_GREEN, LV_PART_INDICATOR);
    lv_obj_add_event_cb(sl_v, sys_vol_sl_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *abtns = lv_obj_create(dacard);
    lv_obj_set_size(abtns, 426, 42);
    lv_obj_set_style_bg_opa(abtns, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(abtns, 0, 0);
    lv_obj_set_style_pad_all(abtns, 0, 0);
    lv_obj_set_style_pad_column(abtns, 12, 0);
    lv_obj_remove_flag(abtns, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(abtns, LV_FLEX_FLOW_ROW);

    lv_obj_t *btst = lv_button_create(abtns);
    lv_obj_add_style(btst, &g_st_btn_ghost, 0);
    lv_obj_set_flex_grow(btst, 1);
    lv_obj_set_height(btst, 38);
    lv_obj_add_event_cb(btst, sys_test_snd_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lt = lv_label_create(btst);
    lv_label_set_text(lt, LV_SYMBOL_AUDIO " Test Alarm Sound");
    lv_obj_center(lt);

    lv_obj_t *bstp = lv_button_create(abtns);
    lv_obj_add_style(bstp, &g_st_btn_danger, 0);
    lv_obj_set_flex_grow(bstp, 1);
    lv_obj_set_height(bstp, 38);
    lv_obj_add_event_cb(bstp, sys_stop_snd_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ls = lv_label_create(bstp);
    lv_label_set_text(ls, LV_SYMBOL_MUTE " Stop Audio");
    lv_obj_center(ls);

    /* 3. Hardware Topology & Storage Card */
    lv_obj_t *hwcard = lv_obj_create(body);
    lv_obj_add_style(hwcard, &g_st_card, 0);
    lv_obj_set_size(hwcard, 456, 212);
    lv_obj_remove_flag(hwcard, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(hwcard, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(hwcard, 8, 0);

    lv_obj_t *hhdr = lv_label_create(hwcard);
    lv_label_set_text(hhdr, "HARDWARE & BUS TOPOLOGY");
    lv_obj_set_style_text_font(hhdr, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(hhdr, UI_COL_CYAN, 0);

    g_lbl_sd = lv_label_create(hwcard);
    lv_label_set_text(g_lbl_sd, "Recorded SD Alerts: 0");
    lv_obj_set_style_text_font(g_lbl_sd, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(g_lbl_sd, UI_COL_TEXT, 0);

    lv_obj_t *hw_info = lv_label_create(hwcard);
    lv_label_set_text(hw_info,
                      "• I2C1 (P9.3/P9.2): AHT20 Temp/Humi Sensor\n"
                      "• I2C2 (P13.1/P13.2): ST7102 Touch Panel (IRQ P17.2)\n"
                      "• SDHC0 / SDIO: CYW55500 Wi-Fi 6 + WHD In-Memory\n"
                      "• SDHC1: /sdcard FATFS  |  ES8388 I2S Audio\n"
                      "• AI & GPU: Ethos-U55 NPU + GFXSS VG-Lite 2.5D");
    lv_obj_set_style_text_font(hw_info, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(hw_info, UI_COL_MUTED, 0);
    lv_obj_set_style_text_line_space(hw_info, 6, 0);

    ui_screen_system_update();
    return scr;
}

void ui_screen_system_update(void)
{
    dualcore_status_t dc;
    uint32_t up_s = (uint32_t)(rt_tick_get() / RT_TICK_PER_SECOND);

    ui_update_chrome(&g_chrome);
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
