#include <rtthread.h>
#include <stdio.h>
#include <math.h>
#include "ui_theme.h"
#include "ui_manager.h"
#include "usbh_uvc_display_hook.h"
#include "uvc_ai_app.h"
#include "alert_manager.h"
#include "wifi_stream.h"
#include "sd_assets.h"

static ui_chrome_t g_chrome;
static lv_obj_t *g_ai_hub_view;
static lv_obj_t *g_ai_work_view;
static lv_obj_t *g_btn_toggle;
static lv_obj_t *g_lbl_toggle;
static lv_obj_t *g_cam_frame_box;
static lv_obj_t *g_cam_img;
static lv_obj_t *g_lbl_cam_hint;
static lv_obj_t *g_banner_danger;
static lv_obj_t *g_badge_ai_fps;
static lv_obj_t *g_lbl_ai_fps;
static lv_obj_t *g_bar_risk;
static lv_obj_t *g_bar_hold;
static lv_obj_t *g_stats_card;
static lv_obj_t *g_controls_card;
static lv_obj_t *g_fatigue_controls;
static lv_obj_t *g_lbl_risk;
static lv_obj_t *g_lbl_metrics;
static lv_obj_t *g_sl_thresh;
static lv_obj_t *g_lbl_thresh_val;
static lv_obj_t *g_lbl_hold_val;
static lv_obj_t *g_lbl_vol_val;
static lv_obj_t *g_lbl_logs;

static uint32_t g_last_cam_seq = 0U;
static uint32_t g_last_log_danger_cnt = 0xFFFFFFFFU;
static uint32_t g_last_metrics_update;
static uvc_ai_mode_t g_ui_synced_mode = (uvc_ai_mode_t)0xFF;
static bool g_in_work_view = false;
static int g_pending_direct_mode = -1;

enum
{
    AI_UI_REQ_NONE = 0,
    AI_UI_REQ_HUB,
    AI_UI_REQ_BALL,
    AI_UI_REQ_FATIGUE,
    AI_UI_REQ_START,
    AI_UI_REQ_STOP,
};
static volatile uint8_t g_ai_ui_req = AI_UI_REQ_NONE;

static void fatigue_refresh_logs(void);
static void fatigue_sync_mode_widgets(uvc_ai_mode_t mode);
static void fatigue_enter_project(uvc_ai_mode_t mode);
static void fatigue_return_to_hub(void);

static lv_image_dsc_t g_cam_img_dsc = {
    .header = {
        .magic = LV_IMAGE_HEADER_MAGIC,
        .cf = LV_COLOR_FORMAT_RGB565,
        .flags = LV_IMAGE_FLAGS_MODIFIABLE,
        .w = 320,
        .h = 240,
        .stride = 320 * 2,
        .reserved_2 = 0,
    },
    .data_size = 320U * 240U * 2U,
    .data = NULL,
    .reserved = NULL,
};

static void fatigue_back_hub_cb(lv_event_t *e) { (void)e; fatigue_return_to_hub(); }

static void fatigue_toggle_cam_cb(lv_event_t *e)
{
    (void)e;
    if (uvc_app_is_running())
    {
        (void)uvc_app_stop();
    }
    else
    {
        (void)uvc_app_start_monitor();
    }
}

static void fatigue_entry_card_cb(lv_event_t *e)
{
    uvc_ai_mode_t mode = (uvc_ai_mode_t)(intptr_t)lv_event_get_user_data(e);
    fatigue_enter_project(mode);
}

static void fatigue_snap_cb(lv_event_t *e)
{
    (void)e;
    (void)wifi_stream_push_snapshot("fatigue_snap.jpg");
}

static void fatigue_slider_thresh_cb(lv_event_t *e)
{
    int v = lv_slider_get_value(lv_event_get_target(e));
    if (uvc_ai_app_get_mode() == UVC_AI_MODE_STEEL_BALL)
    {
        uvc_ai_set_conf_threshold((float)v / 100.0f);
        lv_label_set_text_fmt(g_lbl_thresh_val, "Conf: %d%%", v);
    }
    else
    {
        alert_set_threshold((float)v / 100.0f);
        lv_label_set_text_fmt(g_lbl_thresh_val, "Thresh: %d%%", v);
    }
}

static void fatigue_sync_mode_widgets(uvc_ai_mode_t mode)
{
    if (mode == g_ui_synced_mode) return;
    g_ui_synced_mode = mode;

    bool is_ball = (mode == UVC_AI_MODE_STEEL_BALL);
    if (is_ball)
    {
        lv_obj_add_flag(g_banner_danger, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(g_bar_hold, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(g_fatigue_controls, LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
        lv_obj_remove_flag(g_bar_hold, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(g_fatigue_controls, LV_OBJ_FLAG_HIDDEN);
        fatigue_refresh_logs();
    }
    lv_obj_set_height(g_stats_card, is_ball ? 80 : 102);
    lv_obj_set_height(g_controls_card, is_ball ? 58 : 250);
    if (g_chrome.lbl_title != NULL)
        ui_set_label_text(g_chrome.lbl_title, g_in_work_view ? (is_ball ? "钢球监测" : "疲劳监测") : "AI 检测");

    if (g_btn_toggle != NULL && !uvc_app_is_running())
    {
        lv_obj_set_style_bg_color(g_btn_toggle, is_ball ? UI_COL_GREEN : UI_COL_CYAN, LV_PART_MAIN);
        lv_obj_set_style_text_color(g_lbl_toggle, lv_color_hex(0x06101F), LV_PART_MAIN);
    }

    if (g_sl_thresh != NULL && g_lbl_thresh_val != NULL)
    {
        if (is_ball)
        {
            int v = (int)(uvc_ai_get_conf_threshold() * 100.0f);
            lv_slider_set_range(g_sl_thresh, 20, 95);
            lv_slider_set_value(g_sl_thresh, v, LV_ANIM_OFF);
            lv_label_set_text_fmt(g_lbl_thresh_val, "Conf: %d%%", v);
        }
        else
        {
            int v = (int)(alert_get_threshold() * 100.0f);
            lv_slider_set_range(g_sl_thresh, 50, 95);
            lv_slider_set_value(g_sl_thresh, v, LV_ANIM_OFF);
            lv_label_set_text_fmt(g_lbl_thresh_val, "Thresh: %d%%", v);
        }
    }
}

static void fatigue_slider_hold_cb(lv_event_t *e)
{
    int v = lv_slider_get_value(lv_event_get_target(e));
    uvc_ai_app_set_persist_ms((uint32_t)v);
    lv_label_set_text_fmt(g_lbl_hold_val, "Hold: %.1fs", (double)(v / 1000.0f));
}

static void fatigue_slider_vol_cb(lv_event_t *e)
{
    int v = lv_slider_get_value(lv_event_get_target(e));
    alert_set_volume(v);
    lv_label_set_text_fmt(g_lbl_vol_val, "Vol: %d", v);
}

static void fatigue_refresh_logs(void)
{
    char buf[384];
    (void)alert_read_recent_logs(buf, sizeof(buf), 4);
    lv_label_set_text(g_lbl_logs, buf);
}

static void fatigue_btn_refresh_logs_cb(lv_event_t *e) { (void)e; fatigue_refresh_logs(); }
static void fatigue_btn_clear_logs_cb(lv_event_t *e)   { (void)e; (void)alert_clear_logs(); fatigue_refresh_logs(); }

static lv_obj_t *make_slider_row(lv_obj_t *parent, const char *init_lbl,
                                 int min_v, int max_v, int cur_v,
                                 lv_event_cb_t cb, lv_obj_t **lbl_out)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, 430, 32);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, init_lbl);
    lv_obj_set_width(lbl, 118);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl, UI_COL_TEXT, 0);

    lv_obj_t *sl = lv_slider_create(row);
    lv_obj_set_size(sl, 290, 10);
    lv_slider_set_range(sl, min_v, max_v);
    lv_slider_set_value(sl, cur_v, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(sl, lv_color_hex(0x1E293B), LV_PART_MAIN);
    lv_obj_set_style_bg_color(sl, UI_COL_CYAN, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sl, UI_COL_TEXT, LV_PART_KNOB);
    lv_obj_add_event_cb(sl, cb, LV_EVENT_VALUE_CHANGED, NULL);

    if (lbl_out) *lbl_out = lbl;
    return sl;
}

static void make_hub_card(lv_obj_t *parent, const char *icon, const char *title,
                          const char *badge_txt, const char *desc,
                          lv_color_t accent, uint32_t bg_hex, uvc_ai_mode_t mode)
{
    const lv_font_t *f_full  = ui_font_zh_full();
    const lv_font_t *f_small = ui_font_zh_small();

    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_add_style(card, &g_st_card, 0);
    lv_obj_set_size(card, 456, 210);
    lv_obj_set_style_bg_color(card, lv_color_hex(bg_hex), 0);
    lv_obj_set_style_border_color(card, accent, 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_pad_all(card, 18, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_add_event_cb(card, fatigue_entry_card_cb, LV_EVENT_CLICKED, (void *)(intptr_t)mode);

    lv_obj_t *top = lv_obj_create(card);
    lv_obj_remove_style_all(top);
    lv_obj_set_size(top, 416, 36);
    lv_obj_remove_flag(top, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *tleft = lv_obj_create(top);
    lv_obj_remove_style_all(tleft);
    lv_obj_set_size(tleft, LV_SIZE_CONTENT, 32);
    lv_obj_remove_flag(tleft, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(tleft, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tleft, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(tleft, 10, 0);

    lv_obj_t *ico = lv_label_create(tleft);
    lv_label_set_text(ico, icon);
    lv_obj_set_style_text_font(ico, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(ico, accent, 0);

    lv_obj_t *lbl_t = lv_label_create(tleft);
    lv_label_set_text(lbl_t, title);
    lv_obj_set_style_text_font(lbl_t, f_full, 0);
    lv_obj_set_style_text_color(lbl_t, lv_color_hex(0xFFFFFF), 0);

    lv_obj_t *pill = lv_obj_create(top);
    lv_obj_add_style(pill, &g_st_pill, 0);
    lv_obj_set_size(pill, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_border_color(pill, accent, 0);
    lv_obj_remove_flag(pill, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *plbl = lv_label_create(pill);
    lv_label_set_text(plbl, badge_txt);
    lv_obj_set_style_text_font(plbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(plbl, accent, 0);

    lv_obj_t *ldesc = lv_label_create(card);
    lv_obj_set_width(ldesc, 416);
    lv_label_set_long_mode(ldesc, LV_LABEL_LONG_WRAP);
    lv_label_set_text(ldesc, desc);
    lv_obj_set_style_text_font(ldesc, f_small, 0);
    lv_obj_set_style_text_color(ldesc, UI_COL_MUTED, 0);

    lv_obj_t *bot = lv_obj_create(card);
    lv_obj_remove_style_all(bot);
    lv_obj_set_size(bot, 416, 28);
    lv_obj_remove_flag(bot, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(bot, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bot, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *lhint = lv_label_create(bot);
    lv_label_set_text(lhint, "独立入口 · 手动启动");
    lv_obj_set_style_text_font(lhint, f_small, 0);
    lv_obj_set_style_text_color(lhint, UI_COL_MUTED, 0);

    lv_obj_t *lgo = lv_label_create(bot);
    lv_label_set_text(lgo, "进入  " LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_font(lgo, f_full, 0);
    lv_obj_set_style_text_color(lgo, accent, 0);
}

lv_obj_t *ui_screen_fatigue_create(void)
{
    const lv_font_t *f_full  = ui_font_zh_full();
    const lv_font_t *f_small = ui_font_zh_small();

    lv_obj_t *scr = lv_obj_create(NULL);
    ui_create_chrome(scr, "AI 检测", UI_SCR_FATIGUE, &g_chrome);

    lv_obj_t *body = g_chrome.body;
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(body, 0, 0);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_AUTO);

    /* 1. Independent AI Hub View (two standalone project cards, never auto-starts UVC/AI) */
    g_ai_hub_view = lv_obj_create(body);
    lv_obj_remove_style_all(g_ai_hub_view);
    lv_obj_set_size(g_ai_hub_view, 456, 640);
    lv_obj_remove_flag(g_ai_hub_view, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(g_ai_hub_view, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g_ai_hub_view, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(g_ai_hub_view, 18, 0);

    lv_obj_t *hub_hdr = lv_label_create(g_ai_hub_view);
    lv_label_set_text(hub_hdr, "选择 AI 检测项目（进入后点击启动）");
    lv_obj_set_style_text_font(hub_hdr, f_small, 0);
    lv_obj_set_style_text_color(hub_hdr, UI_COL_MUTED, 0);

    make_hub_card(g_ai_hub_view, LV_SYMBOL_EYE_OPEN, "钢球监测", "Ethos-U55",
                  "实时检测定位钢球目标并计数\n进入后点击「启动检测」开始识别",
                  UI_COL_GREEN, 0x10252C, UVC_AI_MODE_STEEL_BALL);

    make_hub_card(g_ai_hub_view, LV_SYMBOL_WARNING, "疲劳监测", "YOLOv5-N",
                  "闭眼置信度与持续时长预警及声光告警\n进入后点击「启动检测」开始识别",
                  UI_COL_CYAN, 0x132238, UVC_AI_MODE_FATIGUE);

    lv_obj_t *hub_foot = lv_label_create(g_ai_hub_view);
    lv_label_set_text(hub_foot, "左滑返回表盘");
    lv_obj_set_style_text_font(hub_foot, f_small, 0);
    lv_obj_set_style_text_color(hub_foot, UI_COL_MUTED, 0);

    /* 2. Project Work View (hidden until user enters a project) */
    g_ai_work_view = lv_obj_create(body);
    lv_obj_remove_style_all(g_ai_work_view);
    lv_obj_set_size(g_ai_work_view, 456, LV_SIZE_CONTENT);
    lv_obj_remove_flag(g_ai_work_view, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(g_ai_work_view, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(g_ai_work_view, 8, 0);
    lv_obj_add_flag(g_ai_work_view, LV_OBJ_FLAG_HIDDEN);

    /* Action Toolbar */
    lv_obj_t *bar = lv_obj_create(g_ai_work_view);
    lv_obj_set_size(bar, 456, 44);
    lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_set_style_pad_column(bar, 8, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);

    lv_obj_t *b_back = lv_button_create(bar);
    lv_obj_add_style(b_back, &g_st_btn_ghost, 0);
    lv_obj_set_size(b_back, 92, 42);
    lv_obj_add_event_cb(b_back, fatigue_back_hub_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l_back = lv_label_create(b_back);
    lv_label_set_text(l_back, LV_SYMBOL_LEFT " 入口");
    lv_obj_set_style_text_font(l_back, f_small, 0);
    lv_obj_center(l_back);

    g_btn_toggle = lv_button_create(bar);
    lv_obj_add_style(g_btn_toggle, &g_st_btn_primary, 0);
    lv_obj_set_flex_grow(g_btn_toggle, 1);
    lv_obj_set_height(g_btn_toggle, 42);
    lv_obj_add_event_cb(g_btn_toggle, fatigue_toggle_cam_cb, LV_EVENT_CLICKED, NULL);
    g_lbl_toggle = lv_label_create(g_btn_toggle);
    lv_label_set_text(g_lbl_toggle, LV_SYMBOL_PLAY " 启动检测");
    lv_obj_set_style_text_font(g_lbl_toggle, f_full, 0);
    lv_obj_center(g_lbl_toggle);

    lv_obj_t *b_snap = lv_button_create(bar);
    lv_obj_add_style(b_snap, &g_st_btn_ghost, 0);
    lv_obj_set_size(b_snap, 92, 42);
    lv_obj_add_event_cb(b_snap, fatigue_snap_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l_snap = lv_label_create(b_snap);
    lv_label_set_text(l_snap, LV_SYMBOL_IMAGE " 抓拍");
    lv_obj_set_style_text_font(l_snap, f_small, 0);
    lv_obj_center(l_snap);

    /* Camera Viewport Card (440x330 scaled from 320x240 RGB565 Non-Cacheable stage) */
    g_cam_frame_box = lv_obj_create(g_ai_work_view);
    lv_obj_set_size(g_cam_frame_box, 456, 342);
    lv_obj_set_style_bg_color(g_cam_frame_box, lv_color_hex(0x050811), 0);
    lv_obj_set_style_bg_opa(g_cam_frame_box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(g_cam_frame_box, UI_COL_BORDER, 0);
    lv_obj_set_style_border_width(g_cam_frame_box, 2, 0);
    lv_obj_set_style_radius(g_cam_frame_box, 12, 0);
    lv_obj_set_style_pad_all(g_cam_frame_box, 0, 0);
    lv_obj_remove_flag(g_cam_frame_box, LV_OBJ_FLAG_SCROLLABLE);

    uint16_t w = 320, h = 240;
    const uint16_t *stage = uvc_display_get_stage_rgb565(&w, &h, NULL);
    g_cam_img_dsc.data = (const uint8_t *)stage;

    g_cam_img = lv_image_create(g_cam_frame_box);
    lv_image_set_src(g_cam_img, &g_cam_img_dsc);
    lv_image_set_pivot(g_cam_img, 0, 0);
    lv_image_set_scale(g_cam_img, 352); /* 320*352/256 = 440, 240*352/256 = 330 */
    lv_obj_set_size(g_cam_img, 440, 330);
    lv_obj_set_pos(g_cam_img, 8, 6);
    lv_obj_add_flag(g_cam_img, LV_OBJ_FLAG_HIDDEN);

    g_lbl_cam_hint = lv_label_create(g_cam_frame_box);
    lv_label_set_text(g_lbl_cam_hint, LV_SYMBOL_VIDEO "\nCamera Standby\nTap 'Start AI Cam' or Press KEY P8.3");
    lv_obj_set_style_text_align(g_lbl_cam_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(g_lbl_cam_hint, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(g_lbl_cam_hint, UI_COL_MUTED, 0);
    lv_obj_center(g_lbl_cam_hint);

    g_banner_danger = lv_obj_create(g_cam_frame_box);
    lv_obj_set_size(g_banner_danger, 440, 36);
    lv_obj_set_pos(g_banner_danger, 8, 6);
    lv_obj_set_style_bg_color(g_banner_danger, UI_COL_RED, 0);
    lv_obj_set_style_bg_opa(g_banner_danger, LV_OPA_90, 0);
    lv_obj_set_style_border_width(g_banner_danger, 0, 0);
    lv_obj_set_style_radius(g_banner_danger, 8, 0);
    lv_obj_remove_flag(g_banner_danger, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_banner_danger, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *btxt = lv_label_create(g_banner_danger);
    lv_label_set_text(btxt, LV_SYMBOL_WARNING "  DROWSY DANGER DETECTED — PLEASE REST!");
    lv_obj_set_style_text_color(btxt, UI_COL_TEXT, 0);
    lv_obj_set_style_text_font(btxt, &lv_font_montserrat_14, 0);
    lv_obj_center(btxt);

    g_badge_ai_fps = lv_obj_create(g_cam_frame_box);
    lv_obj_set_size(g_badge_ai_fps, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(g_badge_ai_fps, LV_ALIGN_BOTTOM_RIGHT, -14, -12);
    lv_obj_set_style_bg_color(g_badge_ai_fps, lv_color_hex(0x0B1324), 0);
    lv_obj_set_style_bg_opa(g_badge_ai_fps, LV_OPA_80, 0);
    lv_obj_set_style_border_color(g_badge_ai_fps, UI_COL_CYAN, 0);
    lv_obj_set_style_border_width(g_badge_ai_fps, 1, 0);
    lv_obj_set_style_radius(g_badge_ai_fps, 12, 0);
    lv_obj_set_style_pad_hor(g_badge_ai_fps, 10, 0);
    lv_obj_set_style_pad_ver(g_badge_ai_fps, 4, 0);
    lv_obj_remove_flag(g_badge_ai_fps, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_badge_ai_fps, LV_OBJ_FLAG_HIDDEN);

    g_lbl_ai_fps = lv_label_create(g_badge_ai_fps);
    lv_label_set_text(g_lbl_ai_fps, "AI: -- FPS");
    lv_obj_set_style_text_font(g_lbl_ai_fps, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(g_lbl_ai_fps, UI_COL_CYAN, 0);

    /* Risk Telemetry & Persistence Meter Card */
    lv_obj_t *tcard = lv_obj_create(g_ai_work_view);
    g_stats_card = tcard;
    lv_obj_add_style(tcard, &g_st_card, 0);
    lv_obj_set_size(tcard, 456, 102);
    lv_obj_remove_flag(tcard, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(tcard, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(tcard, 6, 0);

    lv_obj_t *rrow = lv_obj_create(tcard);
    lv_obj_set_size(rrow, 430, 24);
    lv_obj_set_style_bg_opa(rrow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(rrow, 0, 0);
    lv_obj_set_style_pad_all(rrow, 0, 0);
    lv_obj_remove_flag(rrow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(rrow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(rrow, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    g_lbl_risk = lv_label_create(rrow);
    lv_label_set_text(g_lbl_risk, "SAFE  0%");
    lv_obj_set_style_text_font(g_lbl_risk, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(g_lbl_risk, UI_COL_GREEN, 0);

    g_lbl_metrics = lv_label_create(rrow);
    lv_label_set_text(g_lbl_metrics, "AI: 0.0 fps | 0 ms | Cam: 0 fps");
    lv_obj_set_style_text_font(g_lbl_metrics, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(g_lbl_metrics, UI_COL_MUTED, 0);

    g_bar_risk = lv_bar_create(tcard);
    lv_obj_set_size(g_bar_risk, 430, 14);
    lv_bar_set_range(g_bar_risk, 0, 100);
    lv_obj_set_style_bg_color(g_bar_risk, lv_color_hex(0x1E293B), LV_PART_MAIN);
    lv_obj_set_style_bg_color(g_bar_risk, UI_COL_GREEN, LV_PART_INDICATOR);

    g_bar_hold = lv_bar_create(tcard);
    lv_obj_set_size(g_bar_hold, 430, 10);
    lv_bar_set_range(g_bar_hold, 0, 100);
    lv_obj_set_style_bg_color(g_bar_hold, lv_color_hex(0x1E293B), LV_PART_MAIN);
    lv_obj_set_style_bg_color(g_bar_hold, UI_COL_AMBER, LV_PART_INDICATOR);

    /* Tuning Controls & Persistent SD Logs Card */
    lv_obj_t *ccard = lv_obj_create(g_ai_work_view);
    g_controls_card = ccard;
    lv_obj_add_style(ccard, &g_st_card, 0);
    lv_obj_set_size(ccard, 456, 250);
    lv_obj_remove_flag(ccard, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(ccard, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(ccard, 8, 0);

    int init_th = (int)(alert_get_threshold() * 100.0f);
    int init_hd = (int)uvc_ai_app_get_persist_ms();
    int init_vl = alert_get_volume();
    char sbuf[32];

    rt_snprintf(sbuf, sizeof(sbuf), "Thresh: %d%%", init_th);
    g_sl_thresh = make_slider_row(ccard, sbuf, 50, 95, init_th, fatigue_slider_thresh_cb, &g_lbl_thresh_val);

    g_fatigue_controls = lv_obj_create(ccard);
    lv_obj_set_size(g_fatigue_controls, 430, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(g_fatigue_controls, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g_fatigue_controls, 0, 0);
    lv_obj_set_style_pad_all(g_fatigue_controls, 0, 0);
    lv_obj_set_style_pad_row(g_fatigue_controls, 8, 0);
    lv_obj_set_flex_flow(g_fatigue_controls, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(g_fatigue_controls, LV_OBJ_FLAG_SCROLLABLE);

    rt_snprintf(sbuf, sizeof(sbuf), "Hold: %.1fs", (double)(init_hd / 1000.0f));
    make_slider_row(g_fatigue_controls, sbuf, 1000, 3000, init_hd, fatigue_slider_hold_cb, &g_lbl_hold_val);

    rt_snprintf(sbuf, sizeof(sbuf), "Vol: %d", init_vl);
    make_slider_row(g_fatigue_controls, sbuf, 0, 100, init_vl, fatigue_slider_vol_cb, &g_lbl_vol_val);

    lv_obj_t *lrow = lv_obj_create(g_fatigue_controls);
    lv_obj_set_size(lrow, 430, 30);
    lv_obj_set_style_bg_opa(lrow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(lrow, 0, 0);
    lv_obj_set_style_pad_all(lrow, 0, 0);
    lv_obj_set_style_pad_column(lrow, 8, 0);
    lv_obj_remove_flag(lrow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(lrow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(lrow, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *lhdr = lv_label_create(lrow);
    lv_label_set_text(lhdr, LV_SYMBOL_SD_CARD " SD Danger Logs (/sdcard/danger_log.txt)");
    lv_obj_set_style_text_font(lhdr, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lhdr, UI_COL_CYAN, 0);
    lv_obj_set_flex_grow(lhdr, 1);

    lv_obj_t *bref = lv_button_create(lrow);
    lv_obj_add_style(bref, &g_st_btn_ghost, 0);
    lv_obj_set_size(bref, 58, 26);
    lv_obj_add_event_cb(bref, fatigue_btn_refresh_logs_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lr = lv_label_create(bref);
    lv_label_set_text(lr, "Read");
    lv_obj_set_style_text_font(lr, &lv_font_montserrat_12, 0);
    lv_obj_center(lr);

    lv_obj_t *bclr = lv_button_create(lrow);
    lv_obj_add_style(bclr, &g_st_btn_danger, 0);
    lv_obj_set_size(bclr, 58, 26);
    lv_obj_add_event_cb(bclr, fatigue_btn_clear_logs_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lc = lv_label_create(bclr);
    lv_label_set_text(lc, "Clear");
    lv_obj_set_style_text_font(lc, &lv_font_montserrat_12, 0);
    lv_obj_center(lc);

    g_lbl_logs = lv_label_create(g_fatigue_controls);
    lv_obj_set_width(g_lbl_logs, 430);
    lv_obj_set_style_text_font(g_lbl_logs, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(g_lbl_logs, UI_COL_MUTED, 0);
    lv_label_set_long_mode(g_lbl_logs, LV_LABEL_LONG_WRAP);
    fatigue_sync_mode_widgets(uvc_ai_app_get_mode());

    return scr;
}

static void fatigue_enter_project(uvc_ai_mode_t mode)
{
    if (uvc_app_is_running())
    {
        (void)uvc_app_stop();
    }
    (void)uvc_ai_app_set_mode(mode);
    g_in_work_view = true;
    lv_obj_add_flag(g_ai_hub_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(g_ai_work_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_scroll_to_y(g_chrome.body, 0, LV_ANIM_OFF);
    g_ui_synced_mode = (uvc_ai_mode_t)0xFF;
    fatigue_sync_mode_widgets(mode);
    if (mode == UVC_AI_MODE_FATIGUE)
    {
        fatigue_refresh_logs();
        g_last_log_danger_cnt = alert_get_danger_count();
    }
}

static void fatigue_return_to_hub(void)
{
    if (uvc_app_is_running())
    {
        (void)uvc_app_stop();
    }
    g_in_work_view = false;
    lv_obj_add_flag(g_ai_work_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(g_ai_hub_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_scroll_to_y(g_chrome.body, 0, LV_ANIM_OFF);
    if (g_chrome.lbl_title != NULL)
        ui_set_label_text(g_chrome.lbl_title, "AI 检测");
}

void ui_screen_fatigue_open_hub(void)
{
    g_pending_direct_mode = -1;
    if (ui_get_active_screen() == UI_SCR_FATIGUE)
        fatigue_return_to_hub();
}

void ui_screen_fatigue_open_mode(int mode)
{
    g_pending_direct_mode = mode;
    if (ui_get_active_screen() == UI_SCR_FATIGUE)
    {
        g_pending_direct_mode = -1;
        fatigue_enter_project((uvc_ai_mode_t)mode);
    }
}

void ui_screen_fatigue_on_enter(void)
{
    if (g_pending_direct_mode >= 0)
    {
        int m = g_pending_direct_mode;
        g_pending_direct_mode = -1;
        fatigue_enter_project((uvc_ai_mode_t)m);
    }
    else
    {
        fatigue_return_to_hub();
    }
}

void ui_screen_fatigue_on_leave(void)
{
    if (uvc_app_is_running())
    {
        (void)uvc_app_stop();
    }
}

bool ui_screen_fatigue_handle_swipe(lv_dir_t dir)
{
    if ((dir == LV_DIR_LEFT || dir == LV_DIR_RIGHT) && g_in_work_view)
    {
        fatigue_return_to_hub();
        return true;
    }
    return false;
}

void ui_screen_fatigue_update(void)
{
    uvc_ai_telemetry_t tel;
    uint32_t cam_fps = 0, disp_fps = 0, drops = 0;
    uint16_t w = 320, h = 240;
    uint32_t seq = 0U;
    char text[64];

    ui_update_chrome(&g_chrome);

    rt_base_t level = rt_hw_interrupt_disable();
    uint8_t areq = g_ai_ui_req;
    g_ai_ui_req = AI_UI_REQ_NONE;
    rt_hw_interrupt_enable(level);

    switch (areq)
    {
    case AI_UI_REQ_HUB:
        fatigue_return_to_hub();
        break;
    case AI_UI_REQ_BALL:
        fatigue_enter_project(UVC_AI_MODE_STEEL_BALL);
        break;
    case AI_UI_REQ_FATIGUE:
        fatigue_enter_project(UVC_AI_MODE_FATIGUE);
        break;
    case AI_UI_REQ_START:
        if (g_in_work_view && !uvc_app_is_running())
            fatigue_toggle_cam_cb(NULL);
        break;
    case AI_UI_REQ_STOP:
        if (uvc_app_is_running())
            (void)uvc_app_stop();
        break;
    default:
        break;
    }

    if (!g_in_work_view)
        return;

    uvc_ai_app_get_telemetry(&tel);
    fatigue_sync_mode_widgets(tel.mode);
    uvc_app_get_fps(&cam_fps, &disp_fps, &drops);
    (void)uvc_display_get_stage_rgb565(&w, &h, &seq);

    if (uvc_app_is_running())
    {
        if (!lv_color_eq(lv_obj_get_style_bg_color(g_btn_toggle, LV_PART_MAIN), UI_COL_RED))
        {
            lv_obj_set_style_bg_color(g_btn_toggle, UI_COL_RED, LV_PART_MAIN);
            lv_obj_set_style_text_color(g_lbl_toggle, UI_COL_TEXT, LV_PART_MAIN);
        }
        ui_set_label_text(g_lbl_toggle, LV_SYMBOL_STOP " 停止检测");
    }
    else
    {
        lv_color_t idle_col = (tel.mode == UVC_AI_MODE_STEEL_BALL) ? UI_COL_GREEN : UI_COL_CYAN;
        if (!lv_color_eq(lv_obj_get_style_bg_color(g_btn_toggle, LV_PART_MAIN), idle_col))
        {
            lv_obj_set_style_bg_color(g_btn_toggle, idle_col, LV_PART_MAIN);
            lv_obj_set_style_text_color(g_lbl_toggle, lv_color_hex(0x06101F), LV_PART_MAIN);
        }
        ui_set_label_text(g_lbl_toggle, LV_SYMBOL_PLAY " 启动检测");
    }

    if (uvc_app_has_frame() && seq > 0U)
    {
        lv_obj_add_flag(g_lbl_cam_hint, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(g_cam_img, LV_OBJ_FLAG_HIDDEN);
        if (seq != g_last_cam_seq)
        {
            g_last_cam_seq = seq;
            lv_image_cache_drop(&g_cam_img_dsc);
            lv_obj_invalidate(g_cam_img);
        }
    }
    else
    {
        lv_obj_add_flag(g_cam_img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(g_lbl_cam_hint, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_text_font(g_lbl_cam_hint, ui_font_zh_full(), 0);
        if (tel.mode == UVC_AI_MODE_STEEL_BALL && !sdcard_is_mounted())
        {
            ui_set_label_text(g_lbl_cam_hint, "钢球模型未加载，请插入TF卡");
        }
        else if (tel.mode == UVC_AI_MODE_STEEL_BALL)
        {
            ui_set_label_text(g_lbl_cam_hint, "钢球监测待机中\n点击上方「启动检测」开始");
        }
        else
        {
            ui_set_label_text(g_lbl_cam_hint, "疲劳监测待机中\n点击上方「启动检测」开始");
        }
    }

    if (tel.alert_active)
        lv_obj_remove_flag(g_banner_danger, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(g_banner_danger, LV_OBJ_FLAG_HIDDEN);

    if (uvc_app_is_running() || tel.running)
    {
        lv_color_t badge_col = (tel.mode == UVC_AI_MODE_STEEL_BALL) ? UI_COL_GREEN : UI_COL_CYAN;
        lv_obj_remove_flag(g_badge_ai_fps, LV_OBJ_FLAG_HIDDEN);
        if (!lv_color_eq(lv_obj_get_style_border_color(g_badge_ai_fps, LV_PART_MAIN), badge_col))
        {
            lv_obj_set_style_border_color(g_badge_ai_fps, badge_col, LV_PART_MAIN);
            lv_obj_set_style_text_color(g_lbl_ai_fps, badge_col, LV_PART_MAIN);
        }
        if (tel.running && tel.ai_ready)
        {
            float fps_disp = isfinite(tel.ai_fps) ? tel.ai_fps : 0.0f;
            rt_snprintf(text, sizeof(text), "AI: %.1f FPS", (double)fps_disp);
        }
        else
            rt_snprintf(text, sizeof(text), "AI: -- FPS");
        ui_set_label_text(g_lbl_ai_fps, text);
    }
    else
    {
        lv_obj_add_flag(g_badge_ai_fps, LV_OBJ_FLAG_HIDDEN);
    }

    lv_color_t frame_color = tel.alert_active ? UI_COL_RED : (tel.running ? UI_COL_CYAN : UI_COL_BORDER);
    if (!lv_color_eq(lv_obj_get_style_border_color(g_cam_frame_box, LV_PART_MAIN), frame_color))
        lv_obj_set_style_border_color(g_cam_frame_box, frame_color, LV_PART_MAIN);

    float raw_conf = (tel.mode == UVC_AI_MODE_STEEL_BALL) ? tel.max_det_conf : tel.max_closed_conf;
    int conf_pct = isfinite(raw_conf) ? (int)(raw_conf * 100.0f) : 0;
    if (conf_pct < 0) conf_pct = 0;
    if (conf_pct > 100) conf_pct = 100;
    lv_bar_set_value(g_bar_risk, conf_pct, LV_ANIM_OFF);

    if (tel.mode == UVC_AI_MODE_STEEL_BALL)
    {
        lv_color_t rc = (tel.det_count > 0U) ? UI_COL_GREEN : UI_COL_CYAN;
        rt_snprintf(text, sizeof(text), "BALLS: %u  MAX: %d%%", (unsigned)tel.det_count, conf_pct);
        ui_set_label_text(g_lbl_risk, text);
        if (!lv_color_eq(lv_obj_get_style_text_color(g_lbl_risk, LV_PART_MAIN), rc))
        {
            lv_obj_set_style_text_color(g_lbl_risk, rc, LV_PART_MAIN);
            lv_obj_set_style_bg_color(g_bar_risk, rc, LV_PART_INDICATOR);
        }
    }
    else
    {
        uint32_t hold_pct = (tel.persist_target_ms > 0U)
                            ? ((tel.persist_ms * 100U) / tel.persist_target_ms)
                            : 0U;
        if (hold_pct > 100U) hold_pct = 100U;
        lv_bar_set_value(g_bar_hold, (int32_t)hold_pct, LV_ANIM_OFF);

        lv_color_t rc = (tel.risk_level >= 2U || tel.alert_active) ? UI_COL_RED
                        : ((tel.risk_level == 1U) ? UI_COL_AMBER : UI_COL_GREEN);
        const char *rtxt = (tel.alert_active || tel.risk_level >= 2U) ? "HIGH RISK"
                           : ((tel.risk_level == 1U) ? "MID RISK" : "SAFE");
        rt_snprintf(text, sizeof(text), "%s  %d%%", rtxt, conf_pct);
        ui_set_label_text(g_lbl_risk, text);
        if (!lv_color_eq(lv_obj_get_style_text_color(g_lbl_risk, LV_PART_MAIN), rc))
        {
            lv_obj_set_style_text_color(g_lbl_risk, rc, LV_PART_MAIN);
            lv_obj_set_style_bg_color(g_bar_risk, rc, LV_PART_INDICATOR);
        }
    }

    if (lv_tick_elaps(g_last_metrics_update) >= 500U)
    {
        float fps_disp = isfinite(tel.ai_fps) ? tel.ai_fps : 0.0f;
        float inf_disp = isfinite(tel.inference_ms) ? tel.inference_ms : 0.0f;
        g_last_metrics_update = lv_tick_get();
        if (tel.running && !tel.ai_ready)
            rt_snprintf(text, sizeof(text), "%s",
                        uvc_ai_app_is_failed()
                            ? ((tel.mode == UVC_AI_MODE_STEEL_BALL) ? "SteelBall TF model missing" : "AI unavailable")
                            : "Loading AI model...");
        else if (tel.mode == UVC_AI_MODE_STEEL_BALL)
            rt_snprintf(text, sizeof(text), "AI: %.1f fps | %.0f ms | Cam: %lu fps",
                        (double)fps_disp, (double)inf_disp, (unsigned long)cam_fps);
        else
            rt_snprintf(text, sizeof(text), "AI: %.1f fps | %.0f ms | Cam:%lu | Alm:%lu",
                        (double)fps_disp, (double)inf_disp, (unsigned long)cam_fps, (unsigned long)tel.danger_count);
        ui_set_label_text(g_lbl_metrics, text);
    }

    if (tel.mode == UVC_AI_MODE_FATIGUE && tel.danger_count != g_last_log_danger_cnt)
    {
        g_last_log_danger_cnt = tel.danger_count;
        fatigue_refresh_logs();
    }
}

static int cmd_ai_ui(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "status") == 0)
    {
        rt_kprintf("ai_ui view=%s mode=%d uvc_running=%d\n",
                   g_in_work_view ? "work" : "hub",
                   (int)uvc_ai_app_get_mode(),
                   uvc_app_is_running() ? 1 : 0);
        return RT_EOK;
    }
    uint8_t req = AI_UI_REQ_NONE;
    if (strcmp(argv[1], "hub") == 0)          req = AI_UI_REQ_HUB;
    else if (strcmp(argv[1], "ball") == 0)    req = AI_UI_REQ_BALL;
    else if (strcmp(argv[1], "fatigue") == 0) req = AI_UI_REQ_FATIGUE;
    else if (strcmp(argv[1], "start") == 0)   req = AI_UI_REQ_START;
    else if (strcmp(argv[1], "stop") == 0)    req = AI_UI_REQ_STOP;
    else
    {
        rt_kprintf("Usage: ai_ui <status|hub|ball|fatigue|start|stop>\n");
        return -RT_EINVAL;
    }
    rt_base_t level = rt_hw_interrupt_disable();
    g_ai_ui_req = req;
    rt_hw_interrupt_enable(level);
    return RT_EOK;
}
MSH_CMD_EXPORT_ALIAS(cmd_ai_ui, ai_ui, Control or query independent AI detection UI);
