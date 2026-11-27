#include <rtthread.h>
#include <stdio.h>
#include <string.h>
#include "ui_theme.h"
#include "ui_manager.h"
#include "wifi_stream.h"

static ui_chrome_t g_chrome;
static lv_obj_t *g_lbl_conn_state;
static lv_obj_t *g_lbl_url;
static lv_obj_t *g_lbl_stream_stats;
static lv_obj_t *g_sw_http;
static lv_obj_t *g_lbl_qual;
static lv_obj_t *g_lbl_fps;
static lv_obj_t *g_ap_list;
static lv_obj_t *g_btn_scan;
static lv_obj_t *g_lbl_scan_btn;
static lv_obj_t *g_btn_manual;

static lv_obj_t *g_modal_pwd;
static lv_obj_t *g_modal_ssid_lbl;
static lv_obj_t *g_ta_ssid;
static lv_obj_t *g_ta_pwd;
static lv_obj_t *g_kb;
static char g_selected_ssid[33];
static rt_bool_t g_was_scanning = RT_FALSE;
static uint32_t g_last_scan_seq = 0U;

static void wifi_scan_btn_cb(lv_event_t *e)
{
    (void)e;
    (void)wifi_mgr_start_scan();
}

static void wifi_disc_btn_cb(lv_event_t *e)
{
    (void)e;
    (void)wifi_mgr_disconnect();
}

static void wifi_http_sw_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    (void)wifi_stream_set_http_enable(lv_obj_has_state(sw, LV_STATE_CHECKED) ? RT_TRUE : RT_FALSE);
}

static void wifi_qual_sl_cb(lv_event_t *e)
{
    int v = lv_slider_get_value(lv_event_get_target(e));
    wifi_stream_set_quality(v);
    lv_label_set_text_fmt(g_lbl_qual, "JPEG Quality: %d%%", v);
}

static void wifi_fps_btn_cb(lv_event_t *e)
{
    int fps = (int)(intptr_t)lv_event_get_user_data(e);
    wifi_stream_set_max_fps(fps);
    lv_label_set_text_fmt(g_lbl_fps, "Max FPS: %d", fps);
}

static void wifi_snap_btn_cb(lv_event_t *e)
{
    (void)e;
    (void)wifi_stream_push_snapshot("wifi_manual.jpg");
}

static void modal_close_cb(lv_event_t *e)
{
    (void)e;
    lv_obj_add_flag(g_modal_pwd, LV_OBJ_FLAG_HIDDEN);
}

static void modal_connect_cb(lv_event_t *e)
{
    (void)e;
    const char *ssid = lv_obj_has_flag(g_ta_ssid, LV_OBJ_FLAG_HIDDEN) ?
                       g_selected_ssid : lv_textarea_get_text(g_ta_ssid);
    const char *pwd = lv_textarea_get_text(g_ta_pwd);
    if (ssid[0] == '\0' || strlen(ssid) > 32U)
    {
        lv_label_set_text(g_modal_ssid_lbl, "请输入有效的网络名称");
        return;
    }
    if (wifi_mgr_connect(ssid, pwd, RT_TRUE) == RT_EOK)
        lv_obj_add_flag(g_modal_pwd, LV_OBJ_FLAG_HIDDEN);
    else
        lv_label_set_text(g_modal_ssid_lbl, "无线模块未就绪");
}

static void kb_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY)
    {
        if (lv_keyboard_get_textarea(g_kb) == g_ta_ssid)
        {
            lv_keyboard_set_textarea(g_kb, g_ta_pwd);
            lv_obj_add_state(g_ta_pwd, LV_STATE_FOCUSED);
        }
        else
            modal_connect_cb(e);
    }
    else if (code == LV_EVENT_CANCEL)
    {
        modal_close_cb(e);
    }
}

static void wifi_edit_field(lv_event_t *event)
{
    lv_keyboard_set_textarea(g_kb, lv_event_get_target(event));
}

static void wifi_manual_cb(lv_event_t *event)
{
    (void)event;
    lv_label_set_text(g_modal_ssid_lbl, "手动连接 Wi-Fi");
    lv_obj_remove_flag(g_ta_ssid, LV_OBJ_FLAG_HIDDEN);
    lv_textarea_set_text(g_ta_ssid, "");
    lv_textarea_set_text(g_ta_pwd, "");
    lv_keyboard_set_textarea(g_kb, g_ta_ssid);
    lv_obj_remove_flag(g_modal_pwd, LV_OBJ_FLAG_HIDDEN);
}

static void ap_item_click_cb(lv_event_t *e)
{
    const char *ssid = (const char *)lv_event_get_user_data(e);
    if (ssid == NULL) return;
    wifi_ssid_copy_utf8(g_selected_ssid, sizeof(g_selected_ssid), ssid);
    lv_label_set_text_fmt(g_modal_ssid_lbl, "连接 %s", g_selected_ssid);
    lv_obj_add_flag(g_ta_ssid, LV_OBJ_FLAG_HIDDEN);
    lv_textarea_set_text(g_ta_pwd, "");
    lv_keyboard_set_textarea(g_kb, g_ta_pwd);
    lv_obj_remove_flag(g_modal_pwd, LV_OBJ_FLAG_HIDDEN);
}

static void wifi_rebuild_ap_list(void)
{
    static wifi_ap_item_t items[WIFI_MGR_MAX_SCAN_APS];
    static char ssid_store[WIFI_MGR_MAX_SCAN_APS][33];
    const lv_font_t *f_full  = ui_font_zh_full();
    const lv_font_t *f_small = ui_font_zh_small();
    rt_bool_t scanning = RT_FALSE;
    int cnt = wifi_mgr_get_scan_results(items, WIFI_MGR_MAX_SCAN_APS, &scanning);

    lv_obj_clean(g_ap_list);

    if (cnt == 0)
    {
        lv_obj_t *empty = lv_label_create(g_ap_list);
        lv_label_set_text(empty, scanning ? "正在搜索附近网络" : "点击扫描，选择和电脑同一路由器的网络");
        lv_obj_set_width(empty, lv_pct(100));
        lv_obj_set_style_text_font(empty, f_small, 0);
        lv_obj_set_style_text_color(empty, UI_COL_MUTED, 0);
        return;
    }

    for (int i = 0; i < cnt; i++)
    {
        wifi_ssid_copy_utf8(ssid_store[i], sizeof(ssid_store[i]), items[i].ssid);

        lv_obj_t *row = lv_button_create(g_ap_list);
        lv_obj_add_style(row, &g_st_btn_ghost, 0);
        lv_obj_set_size(row, 416, 44);
        lv_obj_set_style_pad_hor(row, 12, 0);
        lv_obj_set_style_pad_ver(row, 4, 0);
        lv_obj_set_style_pad_column(row, 8, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_add_event_cb(row, ap_item_click_cb, LV_EVENT_CLICKED, ssid_store[i]);

        lv_obj_t *sl = lv_label_create(row);
        lv_label_set_text(sl, ssid_store[i]);
        lv_obj_set_style_text_font(sl, f_full, 0);
        lv_obj_set_style_text_color(sl, UI_COL_TEXT, 0);
        lv_obj_set_flex_grow(sl, 1);
        lv_label_set_long_mode(sl, LV_LABEL_LONG_DOT);

        lv_obj_t *rl = lv_label_create(row);
        lv_label_set_text_fmt(rl, "%d dBm", (int)items[i].rssi);
        lv_obj_set_style_text_font(rl, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(rl, (items[i].rssi > -65) ? UI_COL_GREEN : UI_COL_AMBER, 0);
    }
}

lv_obj_t *ui_screen_wifi_create(void)
{
    const lv_font_t *f_full  = ui_font_zh_full();
    const lv_font_t *f_small = ui_font_zh_small();

    lv_obj_t *scr = lv_obj_create(NULL);
    ui_create_chrome(scr, "无线网络与图传", UI_SCR_WIFI, &g_chrome);

    lv_obj_t *body = g_chrome.body;
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(body, 10, 0);

    /* 1. Connection & URL Card */
    lv_obj_t *scard = lv_obj_create(body);
    lv_obj_add_style(scard, &g_st_card, 0);
    lv_obj_set_size(scard, 456, 124);
    lv_obj_remove_flag(scard, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(scard, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(scard, 6, 0);

    g_lbl_conn_state = lv_label_create(scard);
    lv_obj_set_width(g_lbl_conn_state, 430);
    lv_label_set_long_mode(g_lbl_conn_state, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(g_lbl_conn_state, f_full, LV_PART_MAIN);
    lv_label_set_text(g_lbl_conn_state, "无线模块初始化中");
    lv_obj_set_style_text_color(g_lbl_conn_state, UI_COL_CYAN, 0);

    g_lbl_url = lv_label_create(scard);
    lv_label_set_text(g_lbl_url, "HTTP: http://0.0.0.0:8080/stream\nRTSP: rtsp://0.0.0.0:8554/camera");
    lv_obj_set_style_text_font(g_lbl_url, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(g_lbl_url, UI_COL_TEXT, 0);

    g_lbl_stream_stats = lv_label_create(scard);
    lv_label_set_text(g_lbl_stream_stats, "Clients: 0 | 0 FPS | Relay: idle");
    lv_obj_set_style_text_font(g_lbl_stream_stats, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(g_lbl_stream_stats, UI_COL_MUTED, 0);

    /* 2. Stream Controls Card */
    lv_obj_t *ctrl = lv_obj_create(body);
    lv_obj_add_style(ctrl, &g_st_card, 0);
    lv_obj_set_size(ctrl, 456, 174);
    lv_obj_remove_flag(ctrl, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(ctrl, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(ctrl, 8, 0);

    lv_obj_t *row_sw = lv_obj_create(ctrl);
    lv_obj_set_size(row_sw, 430, 32);
    lv_obj_set_style_bg_opa(row_sw, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row_sw, 0, 0);
    lv_obj_set_style_pad_all(row_sw, 0, 0);
    lv_obj_remove_flag(row_sw, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row_sw, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row_sw, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *lh = lv_label_create(row_sw);
    lv_label_set_text(lh, "HTTP MJPEG Server (:8080)");
    lv_obj_set_style_text_font(lh, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lh, UI_COL_TEXT, 0);

    g_sw_http = lv_switch_create(row_sw);
    lv_obj_add_state(g_sw_http, LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(g_sw_http, UI_COL_CYAN, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_add_event_cb(g_sw_http, wifi_http_sw_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *row_q = lv_obj_create(ctrl);
    lv_obj_set_size(row_q, 430, 32);
    lv_obj_set_style_bg_opa(row_q, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row_q, 0, 0);
    lv_obj_set_style_pad_all(row_q, 0, 0);
    lv_obj_remove_flag(row_q, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row_q, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row_q, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    g_lbl_qual = lv_label_create(row_q);
    lv_label_set_text_fmt(g_lbl_qual, "JPEG Quality: %d%%", wifi_stream_get_quality());
    lv_obj_set_width(g_lbl_qual, 148);
    lv_obj_set_style_text_font(g_lbl_qual, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(g_lbl_qual, UI_COL_TEXT, 0);

    lv_obj_t *sl_q = lv_slider_create(row_q);
    lv_obj_set_size(sl_q, 260, 10);
    lv_slider_set_range(sl_q, 35, 85);
    lv_slider_set_value(sl_q, wifi_stream_get_quality(), LV_ANIM_OFF);
    lv_obj_set_style_bg_color(sl_q, lv_color_hex(0x1E293B), LV_PART_MAIN);
    lv_obj_set_style_bg_color(sl_q, UI_COL_CYAN, LV_PART_INDICATOR);
    lv_obj_add_event_cb(sl_q, wifi_qual_sl_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *row_fps = lv_obj_create(ctrl);
    lv_obj_set_size(row_fps, 430, 38);
    lv_obj_set_style_bg_opa(row_fps, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row_fps, 0, 0);
    lv_obj_set_style_pad_all(row_fps, 0, 0);
    lv_obj_set_style_pad_column(row_fps, 8, 0);
    lv_obj_remove_flag(row_fps, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row_fps, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row_fps, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    g_lbl_fps = lv_label_create(row_fps);
    lv_label_set_text_fmt(g_lbl_fps, "Max FPS: %d", wifi_stream_get_max_fps());
    lv_obj_set_width(g_lbl_fps, 94);
    lv_obj_set_style_text_font(g_lbl_fps, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(g_lbl_fps, UI_COL_TEXT, 0);

    static const int fps_opts[3] = { 5, 8, 12 };
    for (int i = 0; i < 3; i++)
    {
        lv_obj_t *fb = lv_button_create(row_fps);
        lv_obj_add_style(fb, &g_st_btn_ghost, 0);
        lv_obj_set_size(fb, 56, 32);
        lv_obj_add_event_cb(fb, wifi_fps_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)fps_opts[i]);
        lv_obj_t *fl = lv_label_create(fb);
        lv_label_set_text_fmt(fl, "%d", fps_opts[i]);
        lv_obj_center(fl);
    }

    lv_obj_t *bsnap = lv_button_create(row_fps);
    lv_obj_add_style(bsnap, &g_st_btn_primary, 0);
    lv_obj_set_size(bsnap, 118, 32);
    lv_obj_add_event_cb(bsnap, wifi_snap_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ls = lv_label_create(bsnap);
    lv_label_set_text(ls, LV_SYMBOL_UPLOAD " Push Snap");
    lv_obj_set_style_text_font(ls, &lv_font_montserrat_12, 0);
    lv_obj_center(ls);

    /* 3. Nearby Wi-Fi Networks Scanner */
    lv_obj_t *scancard = lv_obj_create(body);
    lv_obj_add_style(scancard, &g_st_card, 0);
    lv_obj_set_width(scancard, 456);
    lv_obj_set_flex_grow(scancard, 1);
    lv_obj_remove_flag(scancard, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(scancard, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(scancard, 8, 0);

    lv_obj_t *sbar = lv_obj_create(scancard);
    lv_obj_set_size(sbar, 430, 38);
    lv_obj_set_style_bg_opa(sbar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(sbar, 0, 0);
    lv_obj_set_style_pad_all(sbar, 0, 0);
    lv_obj_set_style_pad_column(sbar, 8, 0);
    lv_obj_remove_flag(sbar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(sbar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(sbar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *ap_title = lv_label_create(sbar);
    lv_label_set_text(ap_title, "附近网络");
    lv_obj_set_style_text_font(ap_title, f_full, 0);
    lv_obj_set_style_text_color(ap_title, UI_COL_TEXT, 0);
    lv_obj_set_flex_grow(ap_title, 1);

    g_btn_scan = lv_button_create(sbar);
    lv_obj_add_style(g_btn_scan, &g_st_btn_primary, 0);
    lv_obj_set_size(g_btn_scan, 108, 34);
    lv_obj_set_style_pad_hor(g_btn_scan, 6, 0);
    lv_obj_set_style_pad_ver(g_btn_scan, 0, 0);
    lv_obj_add_event_cb(g_btn_scan, wifi_scan_btn_cb, LV_EVENT_CLICKED, NULL);
    g_lbl_scan_btn = lv_label_create(g_btn_scan);
    lv_label_set_text(g_lbl_scan_btn, "扫描");
    lv_obj_set_style_text_font(g_lbl_scan_btn, f_small, 0);
    lv_obj_center(g_lbl_scan_btn);

    g_btn_manual = lv_button_create(sbar);
    lv_obj_add_style(g_btn_manual, &g_st_btn_ghost, 0);
    lv_obj_set_size(g_btn_manual, 96, 34);
    lv_obj_set_style_pad_hor(g_btn_manual, 6, 0);
    lv_obj_set_style_pad_ver(g_btn_manual, 0, 0);
    lv_obj_add_event_cb(g_btn_manual, wifi_manual_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *manual_label = lv_label_create(g_btn_manual);
    lv_label_set_text(manual_label, "手动");
    lv_obj_set_style_text_font(manual_label, f_small, 0);
    lv_obj_center(manual_label);

    lv_obj_t *bdisc = lv_button_create(sbar);
    lv_obj_add_style(bdisc, &g_st_btn_danger, 0);
    lv_obj_set_size(bdisc, 74, 34);
    lv_obj_set_style_pad_hor(bdisc, 6, 0);
    lv_obj_set_style_pad_ver(bdisc, 0, 0);
    lv_obj_add_event_cb(bdisc, wifi_disc_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ldisc = lv_label_create(bdisc);
    lv_label_set_text(ldisc, "断开");
    lv_obj_set_style_text_font(ldisc, f_small, 0);
    lv_obj_center(ldisc);

    g_ap_list = lv_obj_create(scancard);
    lv_obj_set_width(g_ap_list, 430);
    lv_obj_set_flex_grow(g_ap_list, 1);
    lv_obj_set_style_bg_color(g_ap_list, lv_color_hex(0x0E1525), 0);
    lv_obj_set_style_border_width(g_ap_list, 0, 0);
    lv_obj_set_style_radius(g_ap_list, 10, 0);
    lv_obj_set_style_pad_all(g_ap_list, 6, 0);
    lv_obj_set_style_pad_row(g_ap_list, 6, 0);
    lv_obj_set_flex_flow(g_ap_list, LV_FLEX_FLOW_COLUMN);

    /* Password Entry Modal Overlay + On-Screen Keyboard */
    g_modal_pwd = lv_obj_create(scr);
    lv_obj_set_size(g_modal_pwd, UI_SCREEN_W, UI_SCREEN_H);
    lv_obj_set_pos(g_modal_pwd, 0, 0);
    lv_obj_set_style_bg_color(g_modal_pwd, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(g_modal_pwd, LV_OPA_70, 0);
    lv_obj_set_style_border_width(g_modal_pwd, 0, 0);
    lv_obj_remove_flag(g_modal_pwd, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_modal_pwd, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *dlg = lv_obj_create(g_modal_pwd);
    lv_obj_add_style(dlg, &g_st_card, 0);
    lv_obj_set_size(dlg, 436, LV_SIZE_CONTENT);
    lv_obj_align(dlg, LV_ALIGN_TOP_MID, 0, 96);
    lv_obj_remove_flag(dlg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(dlg, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(dlg, 12, 0);

    g_modal_ssid_lbl = lv_label_create(dlg);
    lv_label_set_text(g_modal_ssid_lbl, "连接 Wi-Fi");
    lv_obj_set_width(g_modal_ssid_lbl, 408);
    lv_label_set_long_mode(g_modal_ssid_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(g_modal_ssid_lbl, f_full, 0);
    lv_obj_set_style_text_color(g_modal_ssid_lbl, UI_COL_CYAN, 0);

    g_ta_ssid = lv_textarea_create(dlg);
    lv_obj_set_size(g_ta_ssid, 408, 42);
    lv_obj_set_style_pad_ver(g_ta_ssid, 6, 0);
    lv_textarea_set_one_line(g_ta_ssid, true);
    lv_textarea_set_max_length(g_ta_ssid, 32);
    lv_textarea_set_placeholder_text(g_ta_ssid, "网络名称");
    lv_obj_set_style_text_font(g_ta_ssid, f_full, 0);
    lv_obj_add_event_cb(g_ta_ssid, wifi_edit_field, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(g_ta_ssid, LV_OBJ_FLAG_HIDDEN);

    g_ta_pwd = lv_textarea_create(dlg);
    lv_obj_set_size(g_ta_pwd, 408, 48);
    lv_obj_set_style_pad_ver(g_ta_pwd, 8, 0);
    lv_textarea_set_one_line(g_ta_pwd, true);
    lv_textarea_set_password_mode(g_ta_pwd, true);
    lv_textarea_set_max_length(g_ta_pwd, RT_WLAN_PASSWORD_MAX_LENGTH);
    lv_textarea_set_placeholder_text(g_ta_pwd, "Wi-Fi 密码");
    lv_obj_set_style_text_font(g_ta_pwd, f_full, 0);
    lv_obj_add_event_cb(g_ta_pwd, wifi_edit_field, LV_EVENT_CLICKED, NULL);

    lv_obj_t *mbtns = lv_obj_create(dlg);
    lv_obj_set_size(mbtns, 408, 46);
    lv_obj_set_style_bg_opa(mbtns, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(mbtns, 0, 0);
    lv_obj_set_style_pad_all(mbtns, 0, 0);
    lv_obj_set_style_pad_column(mbtns, 12, 0);
    lv_obj_set_flex_flow(mbtns, LV_FLEX_FLOW_ROW);

    lv_obj_t *mcanc = lv_button_create(mbtns);
    lv_obj_add_style(mcanc, &g_st_btn_ghost, 0);
    lv_obj_set_flex_grow(mcanc, 1);
    lv_obj_set_height(mcanc, 42);
    lv_obj_set_style_pad_ver(mcanc, 4, 0);
    lv_obj_add_event_cb(mcanc, modal_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lcanc = lv_label_create(mcanc);
    lv_label_set_text(lcanc, "取消");
    lv_obj_set_style_text_font(lcanc, f_full, 0);
    lv_obj_center(lcanc);

    lv_obj_t *mconn = lv_button_create(mbtns);
    lv_obj_add_style(mconn, &g_st_btn_primary, 0);
    lv_obj_set_flex_grow(mconn, 1);
    lv_obj_set_height(mconn, 42);
    lv_obj_set_style_pad_ver(mconn, 4, 0);
    lv_obj_add_event_cb(mconn, modal_connect_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lconn = lv_label_create(mconn);
    lv_label_set_text(lconn, "连接");
    lv_obj_set_style_text_font(lconn, f_full, 0);
    lv_obj_center(lconn);

    g_kb = lv_keyboard_create(g_modal_pwd);
    lv_obj_set_size(g_kb, UI_SCREEN_W, 280);
    lv_obj_align(g_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(g_kb, g_ta_pwd);
    lv_obj_add_event_cb(g_kb, kb_event_cb, LV_EVENT_ALL, NULL);

    wifi_rebuild_ap_list();
    return scr;
}

void ui_screen_wifi_update(void)
{
    wifi_status_t wst;
    ui_update_chrome(&g_chrome);
    wifi_mgr_get_status(&wst);

    if (wst.whd_ready)
    {
        char conn_buf[128];
        lv_obj_remove_state(g_btn_scan, LV_STATE_DISABLED);
        lv_obj_remove_state(g_btn_manual, LV_STATE_DISABLED);
        rt_snprintf(conn_buf, sizeof(conn_buf), "%s  |  %s (%d dBm)",
                    wst.status_msg, wst.ssid[0] ? wst.ssid : "--", wst.rssi);
        ui_set_label_text(g_lbl_conn_state, conn_buf);
    }
    else
    {
        lv_obj_add_state(g_btn_scan, LV_STATE_DISABLED);
        lv_obj_add_state(g_btn_manual, LV_STATE_DISABLED);
        ui_set_label_text(g_lbl_conn_state, "无线模块未就绪");
    }
    lv_obj_set_style_text_color(g_lbl_conn_state,
                                wst.ready ? UI_COL_GREEN : (wst.connected ? UI_COL_AMBER : UI_COL_CYAN), 0);

    lv_label_set_text_fmt(g_lbl_url, "HTTP: http://%s:%u/stream\nRTSP: rtsp://%s:%u/camera",
                          wst.ip_str, WIFI_STREAM_HTTP_PORT, wst.ip_str, WIFI_STREAM_RTSP_PORT);
    lv_label_set_text_fmt(g_lbl_stream_stats,
                          "Clients: %d | Stream: %lu fps (%lu kbps) | Relay: %s:%u (#%lu)",
                          wst.http_clients, (unsigned long)wst.stream_fps, (unsigned long)wst.tx_kbps,
                          wst.relay_host[0] ? wst.relay_host : "unset",
                          (unsigned)wst.relay_port, (unsigned long)wst.relay_sent_count);

    ui_set_label_text(g_lbl_scan_btn, wst.scanning ? "扫描中" : "扫描");

    if ((g_was_scanning != wst.scanning) || (g_last_scan_seq != wst.scan_seq))
    {
        g_was_scanning = wst.scanning;
        g_last_scan_seq = wst.scan_seq;
        wifi_rebuild_ap_list();
    }
}
