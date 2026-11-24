#include <rtthread.h>
#include <stdio.h>
#include "ui_theme.h"
#include "ui_manager.h"
#include "usbh_uvc_display_hook.h"
#include "uvc_ai_app.h"
#include "alert_manager.h"
#include "wifi_stream.h"

static ui_chrome_t g_chrome;
static lv_obj_t *g_btn_toggle;
static lv_obj_t *g_lbl_toggle;
static lv_obj_t *g_cam_frame_box;
static lv_obj_t *g_cam_img;
static lv_obj_t *g_lbl_cam_hint;
static lv_obj_t *g_banner_danger;
static lv_obj_t *g_bar_risk;
static lv_obj_t *g_bar_hold;
static lv_obj_t *g_lbl_risk;
static lv_obj_t *g_lbl_metrics;
static lv_obj_t *g_lbl_thresh_val;
static lv_obj_t *g_lbl_hold_val;
static lv_obj_t *g_lbl_vol_val;
static lv_obj_t *g_lbl_logs;

static uint32_t g_last_cam_seq = 0U;
static uint32_t g_last_log_danger_cnt = 0xFFFFFFFFU;
static uint32_t g_last_metrics_update;

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

static void fatigue_back_cb(lv_event_t *e) { (void)e; ui_switch_to(UI_SCR_HOME); }

static void fatigue_toggle_cam_cb(lv_event_t *e)
{
    (void)e;
    if (uvc_app_is_running())
    {
        (void)uvc_app_stop();
    }
    else
    {
        (void)uvc_app_start(0, 320, 240);
    }
}

static void fatigue_snap_cb(lv_event_t *e)
{
    (void)e;
    (void)wifi_stream_push_snapshot("fatigue_snap.jpg");
}

static void fatigue_slider_thresh_cb(lv_event_t *e)
{
    int v = lv_slider_get_value(lv_event_get_target(e));
    alert_set_threshold((float)v / 100.0f);
    lv_label_set_text_fmt(g_lbl_thresh_val, "Thresh: %d%%", v);
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

lv_obj_t *ui_screen_fatigue_create(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    ui_create_chrome(scr, "FATIGUE MONITOR", UI_SCR_FATIGUE, &g_chrome);

    lv_obj_t *body = g_chrome.body;
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(body, 8, 0);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_AUTO);

    /* Action Toolbar */
    lv_obj_t *bar = lv_obj_create(body);
    lv_obj_set_size(bar, 456, 44);
    lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_set_style_pad_column(bar, 8, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);

    lv_obj_t *b_back = lv_button_create(bar);
    lv_obj_add_style(b_back, &g_st_btn_ghost, 0);
    lv_obj_set_size(b_back, 88, 42);
    lv_obj_add_event_cb(b_back, fatigue_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l_back = lv_label_create(b_back);
    lv_label_set_text(l_back, LV_SYMBOL_LEFT " Back");
    lv_obj_center(l_back);

    g_btn_toggle = lv_button_create(bar);
    lv_obj_add_style(g_btn_toggle, &g_st_btn_primary, 0);
    lv_obj_set_flex_grow(g_btn_toggle, 1);
    lv_obj_set_height(g_btn_toggle, 42);
    lv_obj_add_event_cb(g_btn_toggle, fatigue_toggle_cam_cb, LV_EVENT_CLICKED, NULL);
    g_lbl_toggle = lv_label_create(g_btn_toggle);
    lv_label_set_text(g_lbl_toggle, LV_SYMBOL_PLAY " Start AI Cam");
    lv_obj_set_style_text_font(g_lbl_toggle, &lv_font_montserrat_14, 0);
    lv_obj_center(g_lbl_toggle);

    lv_obj_t *b_snap = lv_button_create(bar);
    lv_obj_add_style(b_snap, &g_st_btn_ghost, 0);
    lv_obj_set_size(b_snap, 96, 42);
    lv_obj_add_event_cb(b_snap, fatigue_snap_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l_snap = lv_label_create(b_snap);
    lv_label_set_text(l_snap, LV_SYMBOL_IMAGE " Snap");
    lv_obj_center(l_snap);

    /* Camera Viewport Card (440x330 scaled from 320x240 RGB565 Non-Cacheable stage) */
    g_cam_frame_box = lv_obj_create(body);
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

    /* Risk Telemetry & Persistence Meter Card */
    lv_obj_t *tcard = lv_obj_create(body);
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
    lv_label_set_text(g_lbl_metrics, "Inf: 0.0 ms | Cam: 0 fps | Alerts: 0");
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
    lv_obj_t *ccard = lv_obj_create(body);
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
    make_slider_row(ccard, sbuf, 50, 95, init_th, fatigue_slider_thresh_cb, &g_lbl_thresh_val);

    rt_snprintf(sbuf, sizeof(sbuf), "Hold: %.1fs", (double)(init_hd / 1000.0f));
    make_slider_row(ccard, sbuf, 1000, 3000, init_hd, fatigue_slider_hold_cb, &g_lbl_hold_val);

    rt_snprintf(sbuf, sizeof(sbuf), "Vol: %d", init_vl);
    make_slider_row(ccard, sbuf, 0, 99, init_vl, fatigue_slider_vol_cb, &g_lbl_vol_val);

    lv_obj_t *lrow = lv_obj_create(ccard);
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

    g_lbl_logs = lv_label_create(ccard);
    lv_obj_set_width(g_lbl_logs, 430);
    lv_obj_set_style_text_font(g_lbl_logs, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(g_lbl_logs, UI_COL_MUTED, 0);
    lv_label_set_long_mode(g_lbl_logs, LV_LABEL_LONG_WRAP);
    fatigue_refresh_logs();

    return scr;
}

void ui_screen_fatigue_on_enter(void)
{
    (void)uvc_app_start_monitor();
    fatigue_refresh_logs();
}

void ui_screen_fatigue_update(void)
{
    uvc_ai_telemetry_t tel;
    uint32_t cam_fps = 0, disp_fps = 0, drops = 0;
    uint16_t w = 320, h = 240;
    uint32_t seq = 0U;
    char text[64];

    ui_update_chrome(&g_chrome);

    uvc_ai_app_get_telemetry(&tel);
    uvc_app_get_fps(&cam_fps, &disp_fps, &drops);
    (void)uvc_display_get_stage_rgb565(&w, &h, &seq);

    if (tel.running)
    {
        if (!lv_color_eq(lv_obj_get_style_bg_color(g_btn_toggle, LV_PART_MAIN), UI_COL_RED))
        {
            lv_obj_set_style_bg_color(g_btn_toggle, UI_COL_RED, LV_PART_MAIN);
            lv_obj_set_style_text_color(g_lbl_toggle, UI_COL_TEXT, LV_PART_MAIN);
        }
        ui_set_label_text(g_lbl_toggle, LV_SYMBOL_STOP " Stop AI Cam");
    }
    else
    {
        if (!lv_color_eq(lv_obj_get_style_bg_color(g_btn_toggle, LV_PART_MAIN), UI_COL_CYAN))
        {
            lv_obj_set_style_bg_color(g_btn_toggle, UI_COL_CYAN, LV_PART_MAIN);
            lv_obj_set_style_text_color(g_lbl_toggle, lv_color_hex(0x06101F), LV_PART_MAIN);
        }
        ui_set_label_text(g_lbl_toggle, LV_SYMBOL_PLAY " Start AI Cam");
    }

    if (tel.running && seq > 0U)
    {
        lv_obj_add_flag(g_lbl_cam_hint, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(g_cam_img, LV_OBJ_FLAG_HIDDEN);
        if (seq != g_last_cam_seq)
        {
            g_last_cam_seq = seq;
            uvc_display_lock_stage();
            lv_image_cache_drop(&g_cam_img_dsc);
            lv_obj_invalidate(g_cam_img);
            uvc_display_unlock_stage();
        }
    }
    else
    {
        lv_obj_add_flag(g_cam_img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(g_lbl_cam_hint, LV_OBJ_FLAG_HIDDEN);
    }

    if (tel.alert_active)
        lv_obj_remove_flag(g_banner_danger, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(g_banner_danger, LV_OBJ_FLAG_HIDDEN);
    lv_color_t frame_color = tel.alert_active ? UI_COL_RED : (tel.running ? UI_COL_CYAN : UI_COL_BORDER);
    if (!lv_color_eq(lv_obj_get_style_border_color(g_cam_frame_box, LV_PART_MAIN), frame_color))
        lv_obj_set_style_border_color(g_cam_frame_box, frame_color, LV_PART_MAIN);

    int conf_pct = (int)(tel.max_closed_conf * 100.0f);
    if (conf_pct < 0) conf_pct = 0;
    if (conf_pct > 100) conf_pct = 100;
    lv_bar_set_value(g_bar_risk, conf_pct, LV_ANIM_OFF);

    uint32_t hold_pct = (tel.persist_target_ms > 0U)
                        ? ((tel.persist_ms * 100U) / tel.persist_target_ms)
                        : 0U;
    if (hold_pct > 100U) hold_pct = 100U;
    lv_bar_set_value(g_bar_hold, (int32_t)hold_pct, LV_ANIM_OFF);

    lv_color_t rc = (tel.risk_level >= 2U || tel.alert_active) ? UI_COL_RED
                    : ((tel.risk_level == 1U) ? UI_COL_AMBER : UI_COL_GREEN);
    const char *rtxt = (tel.alert_active || tel.risk_level >= 2U) ? "HIGH RISK"
                       : ((tel.risk_level == 1U) ? "MID RISK" : "SAFE");
    snprintf(text, sizeof(text), "%s  %d%%", rtxt, conf_pct);
    ui_set_label_text(g_lbl_risk, text);
    if (!lv_color_eq(lv_obj_get_style_text_color(g_lbl_risk, LV_PART_MAIN), rc))
    {
        lv_obj_set_style_text_color(g_lbl_risk, rc, LV_PART_MAIN);
        lv_obj_set_style_bg_color(g_bar_risk, rc, LV_PART_INDICATOR);
    }

    if (lv_tick_elaps(g_last_metrics_update) >= 500U)
    {
        g_last_metrics_update = lv_tick_get();
        snprintf(text, sizeof(text), "Inf: %.1f ms | Cam: %lu fps | Alerts: %lu",
                 (double)tel.inference_ms, (unsigned long)cam_fps, (unsigned long)tel.danger_count);
        ui_set_label_text(g_lbl_metrics, text);
    }

    if (tel.danger_count != g_last_log_danger_cnt)
    {
        g_last_log_danger_cnt = tel.danger_count;
        fatigue_refresh_logs();
    }
}
