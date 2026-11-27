#include <rtthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ui_theme.h"
#include "ui_manager.h"
#include "music_player.h"
#include "sd_assets.h"
#include "ui_music_cover.h"

#define VINYL_REVOLUTION_MS 6000U
#define LYRIC_SLOTS         7
#define LYRIC_ROW_H         34
#define LYRIC_CENTER_Y      73
#define LYRIC_BROWSE_MS     4500U
#define LYRIC_DRAG_SLOP     8

enum
{
    LYRIC_REQ_NONE = 0,
    LYRIC_REQ_SCROLL,
    LYRIC_REQ_TAP,
};

static volatile uint8_t g_lyric_req = LYRIC_REQ_NONE;
static volatile int32_t g_lyric_req_arg = 0;

static ui_chrome_t g_chrome;
static lv_obj_t *g_lbl_title;
static lv_obj_t *g_lbl_meta;
static lv_obj_t *g_slider_prog;
static lv_obj_t *g_lbl_elapsed;
static lv_obj_t *g_lbl_duration;
static lv_obj_t *g_btn_play;
static lv_obj_t *g_lbl_play;
static lv_obj_t *g_mode_btns[MUSIC_MODE_COUNT];
static lv_obj_t *g_mode_lbls[MUSIC_MODE_COUNT];
static lv_obj_t *g_lbl_list_hdr;
static lv_obj_t *g_list_box;
typedef struct {
    lv_obj_t *disc;
    lv_obj_t *art;
    lv_obj_t *fallback;
    lv_obj_t *sweep[3];
} vinyl_view_t;

static vinyl_view_t g_library_vinyl;
static struct {
    ui_chrome_t chrome;
    vinyl_view_t vinyl;
    lv_obj_t *title;
    lv_obj_t *artist;
    lv_obj_t *slider;
    lv_obj_t *elapsed;
    lv_obj_t *duration;
    lv_obj_t *play;
    lv_obj_t *play_symbol;
    lv_obj_t *lyrics;
    lv_obj_t *lyric_focus_bar;
    lv_obj_t *lyric_seek_time;
    lv_obj_t *lyric_strip;
    lv_obj_t *lyric_labels[LYRIC_SLOTS];
    uint8_t lyric_slot_role[LYRIC_SLOTS];
    int32_t scroll_y_px;
    int32_t last_rendered_y;
    int lyric_index;
    bool user_browsing;
    bool drag_active;
    bool drag_moved;
    lv_point_t drag_start_pt;
    lv_point_t drag_last_pt;
    int32_t drag_start_y;
    int32_t drag_vel_y;
    uint32_t browse_until_tick;
    uint32_t seek_hold_until;
} g_detail;
static music_lyrics_t g_lyrics __attribute__((section(".cy_socmem_data")));
static bool g_media_pending;
static bool g_vinyl_playing;
static uint32_t g_vinyl_phase_ms;
static uint32_t g_vinyl_tick;
static uint32_t g_album_pixels[MUSIC_COVER_SIZE * MUSIC_COVER_SIZE]
    __attribute__((section(".cy_socmem_data"), aligned(64)));
static const lv_image_dsc_t g_album_image = {
    .header = {
        .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_ARGB8888,
        .w = MUSIC_COVER_SIZE, .h = MUSIC_COVER_SIZE, .stride = MUSIC_COVER_SIZE * 4U,
    },
    .data_size = sizeof(g_album_pixels),
    .data = (const uint8_t *)g_album_pixels,
};
static lv_obj_t *g_row_btns[MUSIC_MAX_TRACKS] __attribute__((section(".cy_socmem_data")));
static lv_obj_t *g_row_lbls[MUSIC_MAX_TRACKS] __attribute__((section(".cy_socmem_data")));

static uint32_t g_last_rev = 0xFFFFFFFFU;
static uint32_t g_last_library_rev = 0xFFFFFFFFU;
static uint32_t g_last_sd_gen = 0U;
static bool     g_last_sd_mounted;
static int      g_last_active_idx = -2;

static void vinyl_frame(vinyl_view_t *view)
{
    int32_t angle = (int32_t)(g_vinyl_phase_ms * 3600U / VINYL_REVOLUTION_MS);
    if (!lv_obj_has_flag(view->art, LV_OBJ_FLAG_HIDDEN))
        lv_image_set_rotation(view->art, angle);
    for (unsigned i = 0; i < 3; i++)
        lv_arc_set_rotation(view->sweep[i], angle / 10);
}

static void vinyl_set_art(vinyl_view_t *view, bool ready)
{
    if (ready)
    {
        lv_obj_add_flag(view->fallback, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(view->art, LV_OBJ_FLAG_HIDDEN);
        lv_obj_invalidate(view->art);
    }
    else
    {
        lv_obj_add_flag(view->art, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(view->fallback, LV_OBJ_FLAG_HIDDEN);
    }
}

static void vinyl_tick(lv_timer_t *timer)
{
    (void)timer;
    ui_screen_id_t screen = ui_get_active_screen();
    if ((screen != UI_SCR_MUSIC && screen != UI_SCR_NOW_PLAYING) || !g_vinyl_playing)
    {
        g_vinyl_tick = 0;
        return;
    }
    uint32_t now = lv_tick_get();
    if (g_vinyl_tick != 0)
    {
        uint32_t elapsed = (now - g_vinyl_tick) % VINYL_REVOLUTION_MS;
        g_vinyl_phase_ms = (g_vinyl_phase_ms + elapsed) % VINYL_REVOLUTION_MS;
        vinyl_frame(screen == UI_SCR_MUSIC ? &g_library_vinyl : &g_detail.vinyl);
    }
    g_vinyl_tick = now;
}

static void lyric_scroll_anim_cb(void *obj, int32_t y);

static void request_album_art(const music_status_t *st, uint32_t mount_gen)
{
    music_track_info_t track;
    const char *path = NULL;
    if (st->sd_mounted && st->current_index >= 0 &&
        music_player_get_track((unsigned)st->current_index, &track))
        path = track.path;
    vinyl_set_art(&g_library_vinyl, false);
    vinyl_set_art(&g_detail.vinyl, false);
    g_vinyl_phase_ms = 0;
    vinyl_frame(&g_library_vinyl);
    vinyl_frame(&g_detail.vinyl);
    lv_anim_delete(&g_detail, lyric_scroll_anim_cb);
    g_lyrics.count = 0U;
    g_detail.lyric_index = INT32_MIN;
    g_detail.scroll_y_px = 0;
    g_detail.last_rendered_y = INT32_MIN;
    g_detail.user_browsing = false;
    g_detail.drag_active = false;
    g_detail.drag_moved = false;
    g_detail.seek_hold_until = 0U;
    ui_music_cover_request(path, mount_gen);
    g_media_pending = path != NULL;
}

static void format_mmss(uint32_t sec, char *buf, size_t sz)
{
    rt_snprintf(buf, sz, "%02lu:%02lu",
                (unsigned long)(sec / 60U), (unsigned long)(sec % 60U));
}

static void music_vol_btn_cb(lv_event_t *e)
{
    (void)e;
    ui_switch_to(UI_SCR_SYSTEM);
}

static void music_prev_cb(lv_event_t *e)  { (void)e; (void)music_player_prev(); }
static void music_play_cb(lv_event_t *e)  { (void)e; (void)music_player_toggle_pause(); }
static void music_next_cb(lv_event_t *e)  { (void)e; (void)music_player_next(); }
static void music_stop_cb(lv_event_t *e)  { (void)e; (void)music_player_stop(); }
static void detail_open_cb(lv_event_t *e) { (void)e; ui_switch_to(UI_SCR_NOW_PLAYING); }
static void detail_back_cb(lv_event_t *e) { (void)e; ui_switch_to(UI_SCR_MUSIC); }

static void music_mode_cb(lv_event_t *e)
{
    music_play_mode_t m = (music_play_mode_t)(intptr_t)lv_event_get_user_data(e);
    music_player_set_mode(m);
}

static void music_seek_cb(lv_event_t *e)
{
    lv_obj_t *sl = lv_event_get_target(e);
    (void)music_player_seek_permille((uint16_t)lv_slider_get_value(sl));
}

static void music_track_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    music_status_t status;
    music_player_get_status(&status);
    if ((idx == status.current_index && status.state != MUSIC_STATE_STOPPED) ||
        music_player_play_index(idx) == RT_EOK)
        ui_switch_to(UI_SCR_NOW_PLAYING);
}

static void music_refresh_cb(lv_event_t *e)
{
    (void)e;
    (void)music_player_scan();
    ui_screen_music_update();
}

static void rebuild_track_list(const music_status_t *st)
{
    const lv_font_t *f_full  = ui_font_zh_full();
    const lv_font_t *f_small = ui_font_zh_small();

    lv_obj_clean(g_list_box);
    rt_memset(g_row_btns, 0, sizeof(g_row_btns));
    rt_memset(g_row_lbls, 0, sizeof(g_row_lbls));

    char hdr[48];
    rt_snprintf(hdr, sizeof(hdr), "TF 卡曲目 (%lu 首)", (unsigned long)st->track_count);
    ui_set_label_text(g_lbl_list_hdr, hdr);

    if (st->track_count == 0U)
    {
        lv_obj_t *hint = lv_label_create(g_list_box);
        lv_obj_set_width(hint, 420);
        lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_font(hint, f_small, 0);
        lv_obj_set_style_text_color(hint, UI_COL_MUTED, 0);
        lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(hint, st->sd_mounted
                                    ? "未在 /sdcard/music 发现歌曲\n请放入 .mp3 或 .wav 文件后点击「刷新」"
                                    : "未检测到 TF 卡\n请插入带有 /music 曲库的 TF 卡后点击「刷新」");
        g_last_active_idx = -1;
        return;
    }

    for (uint32_t i = 0U; i < st->track_count && i < MUSIC_MAX_TRACKS; i++)
    {
        music_track_info_t tr;
        if (!music_player_get_track(i, &tr)) continue;

        bool active = ((int)i == st->current_index);
        lv_obj_t *row = lv_button_create(g_list_box);
        lv_obj_add_style(row, &g_st_btn_ghost, 0);
        lv_obj_set_size(row, 426, 46);
        lv_obj_set_style_pad_hor(row, 12, 0);
        lv_obj_set_style_radius(row, 10, 0);
        lv_obj_set_style_bg_color(row, active ? lv_color_hex(0x132B49) : UI_COL_CARD_HI, 0);
        lv_obj_set_style_border_color(row, active ? UI_COL_CYAN : UI_COL_BORDER, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_add_event_cb(row, music_track_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        char row_txt[112];
        rt_snprintf(row_txt, sizeof(row_txt), "%02lu. %s", (unsigned long)(i + 1U), tr.title);
        lv_obj_t *lbl = lv_label_create(row);
        lv_label_set_text(lbl, row_txt);
        lv_obj_set_style_text_font(lbl, f_full, 0);
        lv_obj_set_style_text_color(lbl, active ? UI_COL_CYAN : UI_COL_TEXT, 0);
        lv_obj_set_flex_grow(lbl, 1);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);

        lv_obj_t *badge = lv_label_create(row);
        lv_label_set_text(badge, (tr.format == MUSIC_FMT_MP3) ? "MP3" : "WAV");
        lv_obj_set_style_text_font(badge, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(badge, (tr.format == MUSIC_FMT_MP3) ? UI_COL_GREEN : UI_COL_AMBER, 0);

        g_row_btns[i] = row;
        g_row_lbls[i] = lbl;
    }

    g_last_active_idx = st->current_index;
}

static void refresh_active_row_highlight(int cur_idx, uint32_t count)
{
    for (uint32_t i = 0U; i < count && i < MUSIC_MAX_TRACKS; i++)
    {
        if (g_row_btns[i] == NULL || g_row_lbls[i] == NULL) continue;
        bool active = ((int)i == cur_idx);
        lv_obj_set_style_bg_color(g_row_btns[i], active ? lv_color_hex(0x132B49) : UI_COL_CARD_HI, 0);
        lv_obj_set_style_border_color(g_row_btns[i], active ? UI_COL_CYAN : UI_COL_BORDER, 0);
        lv_obj_set_style_text_color(g_row_lbls[i], active ? UI_COL_CYAN : UI_COL_TEXT, 0);
    }
    g_last_active_idx = cur_idx;
}

static lv_obj_t *record_ring(lv_obj_t *parent, int32_t diameter, uint32_t fill, uint32_t edge)
{
    lv_obj_t *ring = lv_obj_create(parent);
    lv_obj_remove_style_all(ring);
    lv_obj_remove_flag(ring, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(ring, diameter, diameter);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(ring, lv_color_hex(fill), 0);
    lv_obj_set_style_bg_opa(ring, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(ring, lv_color_hex(edge), 0);
    lv_obj_set_style_border_width(ring, 1, 0);
    lv_obj_center(ring);
    return ring;
}

static int32_t vinyl_part_size(int part, int diameter)
{
    /* The disc has a one-pixel border on each side of its content box. */
    return lv_pct((part * 100 + (diameter - 2) / 2) / (diameter - 2));
}

static void build_vinyl(lv_obj_t *parent, vinyl_view_t *view, int diameter, int label_size)
{
    lv_obj_t *disc = record_ring(parent, diameter, 0x080B11, 0x465766);
    view->disc = disc;
    for (int groove = diameter - 8, ring = 0; groove > label_size; groove -= 12, ring++)
        record_ring(disc, vinyl_part_size(groove, diameter),
                    ring % 2 ? 0x151D29 : 0x0E1520, 0x293744);

    /* Asymmetric highlights make the platter movement visible without artwork. */
    static const int starts[] = {221, 228, 38};
    static const int ends[] = {282, 267, 66};
    static const uint32_t colors[] = {0x90BDCA, 0x548093, 0xE8A866};
    for (unsigned i = 0; i < 3; i++)
    {
        lv_obj_t *arc = lv_arc_create(disc);
        view->sweep[i] = arc;
        lv_obj_remove_style_all(arc);
        lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        int32_t arc_size = vinyl_part_size(diameter - 16 - (int)i * 15, diameter);
        lv_obj_set_size(arc, arc_size, arc_size);
        lv_arc_set_bg_angles(arc, 0, 360);
        lv_arc_set_angles(arc, starts[i], ends[i]);
        lv_obj_set_style_arc_width(arc, i == 0 ? 5 : 3, LV_PART_INDICATOR);
        lv_obj_set_style_arc_rounded(arc, true, LV_PART_INDICATOR);
        lv_obj_set_style_arc_opa(arc, LV_OPA_COVER, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(arc, lv_color_hex(colors[i]), LV_PART_INDICATOR);
        lv_obj_center(arc);
    }
    lv_obj_t *label = record_ring(disc, vinyl_part_size(label_size, diameter), 0x26384A, 0x557785);
    view->fallback = lv_label_create(label);
    lv_label_set_text(view->fallback, LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_font(view->fallback, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(view->fallback, lv_color_hex(0x9CD8DF), 0);
    lv_obj_center(view->fallback);

    view->art = lv_image_create(disc);
    lv_image_set_src(view->art, &g_album_image);
    lv_image_set_scale(view->art, (uint32_t)label_size * 256U / MUSIC_COVER_SIZE);
    lv_image_set_pivot(view->art, MUSIC_COVER_SIZE / 2, MUSIC_COVER_SIZE / 2);
    lv_obj_center(view->art);
    lv_obj_add_flag(view->art, LV_OBJ_FLAG_HIDDEN);
    vinyl_frame(view);
}

lv_obj_t *ui_screen_music_create(void)
{
    const lv_font_t *f_full  = ui_font_zh_full();
    const lv_font_t *f_small = ui_font_zh_small();

    lv_obj_t *scr = lv_obj_create(NULL);
    ui_create_chrome(scr, "TF 音乐播放器", UI_SCR_MUSIC, &g_chrome);

    lv_obj_t *body = g_chrome.body;
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(body, 10, 0);
    lv_obj_set_style_pad_row(body, 8, 0);

    lv_obj_t *np_card = lv_obj_create(body);
    lv_obj_add_style(np_card, &g_st_card, 0);
    lv_obj_set_size(np_card, 460, 330);
    lv_obj_set_style_pad_all(np_card, 12, 0);
    lv_obj_remove_flag(np_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(np_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(np_card, 6, 0);

    lv_obj_t *hdr_row = lv_obj_create(np_card);
    lv_obj_remove_style_all(hdr_row);
    lv_obj_set_size(hdr_row, 434, 190);
    lv_obj_remove_flag(hdr_row, LV_OBJ_FLAG_SCROLLABLE);
    build_vinyl(hdr_row, &g_library_vinyl, 182, MUSIC_COVER_SIZE);
    lv_obj_align(g_library_vinyl.disc, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_add_flag(g_library_vinyl.disc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(g_library_vinyl.disc, detail_open_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *title_col = lv_obj_create(hdr_row);
    lv_obj_remove_style_all(title_col);
    lv_obj_set_size(title_col, 220, 170);
    lv_obj_align(title_col, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_remove_flag(title_col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(title_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(title_col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(title_col, 8, 0);

    g_lbl_title = lv_label_create(title_col);
    lv_obj_set_width(g_lbl_title, 220);
    lv_obj_set_style_max_height(g_lbl_title, 74, 0);
    lv_label_set_long_mode(g_lbl_title, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(g_lbl_title, f_full, 0);
    lv_obj_set_style_text_color(g_lbl_title, UI_COL_TEXT, 0);
    lv_label_set_text(g_lbl_title, "等待加载歌单");

    g_lbl_meta = lv_label_create(title_col);
    lv_obj_set_width(g_lbl_meta, 220);
    lv_obj_set_style_text_font(g_lbl_meta, f_small, 0);
    lv_obj_set_style_text_color(g_lbl_meta, UI_COL_MUTED, 0);
    lv_label_set_text(g_lbl_meta, "已停止 · 列表循环");

    lv_obj_t *btn_detail = lv_button_create(title_col);
    lv_obj_add_style(btn_detail, &g_st_btn_ghost, 0);
    lv_obj_set_size(btn_detail, 128, 36);
    lv_obj_add_event_cb(btn_detail, detail_open_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *detail_label = lv_label_create(btn_detail);
    lv_obj_set_style_text_font(detail_label, f_small, 0);
    lv_label_set_text(detail_label, "单曲和歌词");
    lv_obj_center(detail_label);

    lv_obj_t *btn_vol = lv_button_create(title_col);
    lv_obj_add_style(btn_vol, &g_st_btn_ghost, 0);
    lv_obj_set_style_border_color(btn_vol, UI_COL_GREEN, 0);
    lv_obj_set_size(btn_vol, 88, 40);
    lv_obj_add_event_cb(btn_vol, music_vol_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l_vol = lv_label_create(btn_vol);
    lv_label_set_text(l_vol, LV_SYMBOL_VOLUME_MAX " 音量");
    lv_obj_set_style_text_font(l_vol, f_small, 0);
    lv_obj_set_style_text_color(l_vol, UI_COL_GREEN, 0);
    lv_obj_center(l_vol);

    /* Progress Bar & Time Labels */
    lv_obj_t *prog_box = lv_obj_create(np_card);
    lv_obj_remove_style_all(prog_box);
    lv_obj_set_size(prog_box, 434, 48);
    lv_obj_remove_flag(prog_box, LV_OBJ_FLAG_SCROLLABLE);
    /* Keep the slider knob inside the parent clip area at every position. */
    lv_obj_set_style_pad_top(prog_box, 6, 0);
    lv_obj_set_style_pad_hor(prog_box, 6, 0);
    lv_obj_set_flex_flow(prog_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(prog_box, 6, 0);

    g_slider_prog = lv_slider_create(prog_box);
    lv_obj_set_size(g_slider_prog, lv_pct(100), 12);
    lv_slider_set_range(g_slider_prog, 0, 1000);
    lv_slider_set_value(g_slider_prog, 0, LV_ANIM_OFF);
    lv_obj_set_ext_click_area(g_slider_prog, 12);
    lv_obj_set_style_bg_color(g_slider_prog, lv_color_hex(0x1E293B), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_slider_prog, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(g_slider_prog, UI_COL_CYAN, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(g_slider_prog, UI_COL_TEXT, LV_PART_KNOB);
    lv_obj_add_event_cb(g_slider_prog, music_seek_cb, LV_EVENT_RELEASED, NULL);

    lv_obj_t *time_row = lv_obj_create(prog_box);
    lv_obj_remove_style_all(time_row);
    lv_obj_set_size(time_row, lv_pct(100), 20);
    lv_obj_remove_flag(time_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(time_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(time_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    g_lbl_elapsed = lv_label_create(time_row);
    lv_label_set_text(g_lbl_elapsed, "00:00");
    lv_obj_set_style_text_font(g_lbl_elapsed, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(g_lbl_elapsed, UI_COL_CYAN, 0);

    g_lbl_duration = lv_label_create(time_row);
    lv_label_set_text(g_lbl_duration, "00:00");
    lv_obj_set_style_text_font(g_lbl_duration, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(g_lbl_duration, UI_COL_MUTED, 0);

    /* Transport Buttons */
    lv_obj_t *ctrl_row = lv_obj_create(np_card);
    lv_obj_remove_style_all(ctrl_row);
    lv_obj_set_size(ctrl_row, 434, 52);
    lv_obj_remove_flag(ctrl_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(ctrl_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(ctrl_row, 8, 0);

    lv_obj_t *b_prev = lv_button_create(ctrl_row);
    lv_obj_add_style(b_prev, &g_st_btn_ghost, 0);
    lv_obj_set_flex_grow(b_prev, 1);
    lv_obj_set_height(b_prev, 50);
    lv_obj_add_event_cb(b_prev, music_prev_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l_prev = lv_label_create(b_prev);
    lv_label_set_text(l_prev, LV_SYMBOL_PREV);
    lv_obj_set_style_text_font(l_prev, &lv_font_montserrat_20, 0);
    lv_obj_center(l_prev);

    g_btn_play = lv_button_create(ctrl_row);
    lv_obj_add_style(g_btn_play, &g_st_btn_primary, 0);
    lv_obj_set_flex_grow(g_btn_play, 2);
    lv_obj_set_height(g_btn_play, 50);
    lv_obj_add_event_cb(g_btn_play, music_play_cb, LV_EVENT_CLICKED, NULL);
    g_lbl_play = lv_label_create(g_btn_play);
    lv_label_set_text(g_lbl_play, LV_SYMBOL_PLAY " 播放");
    lv_obj_set_style_text_font(g_lbl_play, f_full, 0);
    lv_obj_set_style_text_color(g_lbl_play, lv_color_hex(0x06101F), 0);
    lv_obj_center(g_lbl_play);

    lv_obj_t *b_next = lv_button_create(ctrl_row);
    lv_obj_add_style(b_next, &g_st_btn_ghost, 0);
    lv_obj_set_flex_grow(b_next, 1);
    lv_obj_set_height(b_next, 50);
    lv_obj_add_event_cb(b_next, music_next_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l_next = lv_label_create(b_next);
    lv_label_set_text(l_next, LV_SYMBOL_NEXT);
    lv_obj_set_style_text_font(l_next, &lv_font_montserrat_20, 0);
    lv_obj_center(l_next);

    lv_obj_t *b_stop = lv_button_create(ctrl_row);
    lv_obj_add_style(b_stop, &g_st_btn_ghost, 0);
    lv_obj_set_style_border_color(b_stop, UI_COL_RED, 0);
    lv_obj_set_flex_grow(b_stop, 1);
    lv_obj_set_height(b_stop, 50);
    lv_obj_add_event_cb(b_stop, music_stop_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l_stop = lv_label_create(b_stop);
    lv_label_set_text(l_stop, LV_SYMBOL_STOP);
    lv_obj_set_style_text_font(l_stop, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(l_stop, UI_COL_RED, 0);
    lv_obj_center(l_stop);

    /* 2. Play Mode Selector Row (460 x 44) */
    lv_obj_t *mode_row = lv_obj_create(body);
    lv_obj_remove_style_all(mode_row);
    lv_obj_set_size(mode_row, 460, 44);
    lv_obj_remove_flag(mode_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(mode_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(mode_row, 8, 0);

    static const char *const mode_names[MUSIC_MODE_COUNT] = {
        "顺序", "列表循环", "单曲循环", "随机"
    };
    for (unsigned i = 0U; i < MUSIC_MODE_COUNT; i++)
    {
        g_mode_btns[i] = lv_button_create(mode_row);
        lv_obj_add_style(g_mode_btns[i], &g_st_btn_ghost, 0);
        lv_obj_set_flex_grow(g_mode_btns[i], 1);
        lv_obj_set_height(g_mode_btns[i], 42);
        lv_obj_set_style_radius(g_mode_btns[i], 12, 0);
        lv_obj_add_event_cb(g_mode_btns[i], music_mode_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        g_mode_lbls[i] = lv_label_create(g_mode_btns[i]);
        lv_label_set_text(g_mode_lbls[i], mode_names[i]);
        lv_obj_set_style_text_font(g_mode_lbls[i], f_small, 0);
        lv_obj_center(g_mode_lbls[i]);
    }

    lv_obj_t *list_card = lv_obj_create(body);
    lv_obj_add_style(list_card, &g_st_card, 0);
    lv_obj_set_size(list_card, 460, 278);
    lv_obj_set_style_pad_all(list_card, 10, 0);
    lv_obj_remove_flag(list_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(list_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list_card, 8, 0);

    lv_obj_t *lhdr = lv_obj_create(list_card);
    lv_obj_remove_style_all(lhdr);
    lv_obj_set_size(lhdr, 436, 34);
    lv_obj_remove_flag(lhdr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(lhdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(lhdr, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    g_lbl_list_hdr = lv_label_create(lhdr);
    lv_label_set_text(g_lbl_list_hdr, "TF 卡曲目 (0 首)");
    lv_obj_set_style_text_font(g_lbl_list_hdr, f_small, 0);
    lv_obj_set_style_text_color(g_lbl_list_hdr, UI_COL_CYAN, 0);

    lv_obj_t *b_ref = lv_button_create(lhdr);
    lv_obj_add_style(b_ref, &g_st_btn_ghost, 0);
    lv_obj_set_size(b_ref, 88, 32);
    lv_obj_add_event_cb(b_ref, music_refresh_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l_ref = lv_label_create(b_ref);
    lv_label_set_text(l_ref, LV_SYMBOL_REFRESH " 刷新");
    lv_obj_set_style_text_font(l_ref, f_small, 0);
    lv_obj_center(l_ref);

    g_list_box = lv_obj_create(list_card);
    lv_obj_remove_style_all(g_list_box);
    lv_obj_set_width(g_list_box, 436);
    lv_obj_set_flex_grow(g_list_box, 1);
    lv_obj_add_flag(g_list_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(g_list_box, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_flex_flow(g_list_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(g_list_box, 6, 0);

    lv_timer_create(vinyl_tick, 40, NULL);
    return scr;
}

static lv_obj_t *detail_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color,
                              int width, lv_align_t align, int x, int y)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_width(label, width);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_label_set_text(label, "");
    lv_obj_align(label, align, x, y);
    return label;
}

static lv_obj_t *detail_button(lv_obj_t *parent, const char *text, const lv_font_t *font,
                               lv_event_cb_t cb, int width, int x, int y, lv_obj_t **label_out)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_add_style(btn, &g_st_btn_ghost, 0);
    lv_obj_set_size(btn, width, 44);
    lv_obj_align(btn, LV_ALIGN_TOP_MID, x, y);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *label = lv_label_create(btn);
    lv_obj_set_style_text_font(label, font, 0);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    if (label_out) *label_out = label;
    return btn;
}

static void detail_show_lyrics(bool visible)
{
    if (visible == !lv_obj_has_flag(g_detail.lyrics, LV_OBJ_FLAG_HIDDEN)) return;
    if (visible) lv_obj_remove_flag(g_detail.lyrics, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(g_detail.lyrics, LV_OBJ_FLAG_HIDDEN);

    /* Use the space above the playback controls for the artwork on instrumental tracks. */
    int diameter = visible ? 252 : 340;
    unsigned label = visible ? 132U : 178U;
    lv_obj_set_size(g_detail.vinyl.disc, diameter, diameter);
    lv_obj_align(g_detail.vinyl.disc, LV_ALIGN_TOP_MID, 0, visible ? 54 : 86);
    lv_image_set_scale(g_detail.vinyl.art, label * 256U / MUSIC_COVER_SIZE);
    lv_obj_align(g_detail.title, LV_ALIGN_TOP_MID, 0, visible ? 319 : 459);
    lv_obj_align(g_detail.artist, LV_ALIGN_TOP_MID, 0, visible ? 352 : 492);
}

static int32_t detail_lyrics_max_y(void)
{
    return g_lyrics.count > 0U ? (int32_t)(g_lyrics.count - 1U) * LYRIC_ROW_H : 0;
}

static int32_t detail_lyrics_clamp_y(int32_t y)
{
    int32_t max_y = detail_lyrics_max_y();
    if (y < 0) return 0;
    if (y > max_y) return max_y;
    return y;
}

static void detail_lyrics_render(void)
{
    if (g_lyrics.count == 0U)
    {
        lv_obj_add_flag(g_detail.lyric_focus_bar, LV_OBJ_FLAG_HIDDEN);
        for (unsigned i = 0; i < LYRIC_SLOTS; i++)
            ui_set_label_text(g_detail.lyric_labels[i], "");
        return;
    }

    g_detail.scroll_y_px = detail_lyrics_clamp_y(g_detail.scroll_y_px);
    int center_idx = (int)((g_detail.scroll_y_px + LYRIC_ROW_H / 2) / LYRIC_ROW_H);
    if (center_idx < 0) center_idx = 0;
    if (center_idx >= (int)g_lyrics.count) center_idx = (int)g_lyrics.count - 1;
    int32_t sub_px = g_detail.scroll_y_px - (int32_t)center_idx * LYRIC_ROW_H;

    if (g_detail.user_browsing)
    {
        char tbuf[16];
        format_mmss(g_lyrics.lines[center_idx].time_ms / 1000U, tbuf, sizeof(tbuf));
        ui_set_label_text(g_detail.lyric_seek_time, tbuf);
        lv_obj_remove_flag(g_detail.lyric_focus_bar, LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
        lv_obj_add_flag(g_detail.lyric_focus_bar, LV_OBJ_FLAG_HIDDEN);
    }

    const lv_font_t *small = ui_font_zh_small();
    const lv_font_t *full = ui_font_zh_full();
    for (int slot = 0; slot < LYRIC_SLOTS; slot++)
    {
        int idx = center_idx + slot - 3;
        lv_obj_t *lbl = g_detail.lyric_labels[slot];
        int32_t y = LYRIC_CENTER_Y + (slot - 3) * LYRIC_ROW_H - sub_px;
        bool narrow = g_detail.user_browsing && slot == 3;
        int32_t w = narrow ? 352 : 404;
        int32_t x = narrow ? -22 : 0;
        if (lv_obj_get_width(lbl) != w) lv_obj_set_width(lbl, w);
        lv_obj_align(lbl, LV_ALIGN_TOP_MID, x, y);

        const char *text = (idx >= 0 && idx < (int)g_lyrics.count) ? g_lyrics.lines[idx].text : "";
        ui_set_label_text(lbl, text);

        uint8_t role = 0U;
        if (idx >= 0 && idx == g_detail.lyric_index) role = 1U;
        else if (narrow) role = 2U;

        if (role != g_detail.lyric_slot_role[slot])
        {
            g_detail.lyric_slot_role[slot] = role;
            if (role == 1U)
            {
                lv_obj_set_style_text_font(lbl, full, 0);
                lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFCA86), 0);
                lv_label_set_long_mode(lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
            }
            else if (role == 2U)
            {
                lv_obj_set_style_text_font(lbl, full, 0);
                lv_obj_set_style_text_color(lbl, UI_COL_CYAN, 0);
                lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
            }
            else
            {
                lv_obj_set_style_text_font(lbl, small, 0);
                lv_obj_set_style_text_color(lbl, UI_COL_MUTED, 0);
                lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
            }
        }
    }
    g_detail.last_rendered_y = g_detail.scroll_y_px;
}

static void lyric_scroll_anim_cb(void *obj, int32_t y)
{
    (void)obj;
    g_detail.scroll_y_px = detail_lyrics_clamp_y(y);
    detail_lyrics_render();
}

static void detail_lyrics_anim_scroll_to(int32_t target_y, uint32_t dur_ms)
{
    target_y = detail_lyrics_clamp_y(target_y);
    lv_anim_delete(&g_detail, lyric_scroll_anim_cb);
    if (dur_ms == 0U || g_detail.scroll_y_px == target_y)
    {
        g_detail.scroll_y_px = target_y;
        detail_lyrics_render();
        return;
    }
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, &g_detail);
    lv_anim_set_exec_cb(&anim, lyric_scroll_anim_cb);
    lv_anim_set_values(&anim, g_detail.scroll_y_px, target_y);
    lv_anim_set_duration(&anim, dur_ms);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
    lv_anim_start(&anim);
}

static void detail_lyric_seek_to_index(int index)
{
    if (index < 0 || index >= (int)g_lyrics.count) return;
    lv_anim_delete(&g_detail, lyric_scroll_anim_cb);
    g_detail.user_browsing = false;
    g_detail.drag_active = false;
    g_detail.lyric_index = index;
    g_detail.seek_hold_until = lv_tick_get() + 700U;
    (void)music_player_seek_ms(g_lyrics.lines[index].time_ms);
    rt_kprintf("[I/music.ui] lyric seek -> [%d] %lu ms: %s\n",
               index, (unsigned long)g_lyrics.lines[index].time_ms, g_lyrics.lines[index].text);
    detail_lyrics_anim_scroll_to((int32_t)index * LYRIC_ROW_H, 180U);
}

static void detail_lyrics_touch_cb(lv_event_t *e)
{
    if (g_lyrics.count == 0U) return;
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_active();

    if (code == LV_EVENT_PRESSED)
    {
        if (indev == NULL) return;
        lv_point_t pt;
        lv_indev_get_point(indev, &pt);
        lv_anim_delete(&g_detail, lyric_scroll_anim_cb);
        g_detail.drag_start_pt = pt;
        g_detail.drag_last_pt = pt;
        g_detail.drag_start_y = g_detail.scroll_y_px;
        g_detail.drag_vel_y = 0;
        g_detail.drag_active = true;
        g_detail.drag_moved = false;
    }
    else if (code == LV_EVENT_PRESSING)
    {
        if (!g_detail.drag_active || indev == NULL) return;
        lv_point_t pt;
        lv_indev_get_point(indev, &pt);
        int32_t dy_total = pt.y - g_detail.drag_start_pt.y;
        int32_t abs_dy = dy_total < 0 ? -dy_total : dy_total;
        if (!g_detail.drag_moved && abs_dy >= LYRIC_DRAG_SLOP)
        {
            g_detail.drag_moved = true;
            g_detail.user_browsing = true;
        }
        if (g_detail.drag_moved)
        {
            g_detail.drag_vel_y = pt.y - g_detail.drag_last_pt.y;
            g_detail.scroll_y_px = detail_lyrics_clamp_y(g_detail.drag_start_y - dy_total);
            g_detail.browse_until_tick = lv_tick_get() + LYRIC_BROWSE_MS;
            detail_lyrics_render();
        }
        g_detail.drag_last_pt = pt;
    }
    else if (code == LV_EVENT_RELEASED)
    {
        if (!g_detail.drag_active) return;
        g_detail.drag_active = false;
        if (g_detail.drag_moved)
        {
            int32_t end_y = detail_lyrics_clamp_y(g_detail.scroll_y_px - g_detail.drag_vel_y * 4);
            int32_t snapped = ((end_y + LYRIC_ROW_H / 2) / LYRIC_ROW_H) * LYRIC_ROW_H;
            g_detail.browse_until_tick = lv_tick_get() + LYRIC_BROWSE_MS;
            detail_lyrics_anim_scroll_to(snapped, 260U);
        }
        else
        {
            lv_area_t box;
            lv_obj_get_coords(g_detail.lyrics, &box);
            int32_t box_center_y = (box.y1 + box.y2) / 2;
            int32_t content_y = g_detail.scroll_y_px + (g_detail.drag_last_pt.y - box_center_y);
            int tapped = (content_y >= -(LYRIC_ROW_H / 2))
                             ? (int)((content_y + LYRIC_ROW_H / 2) / LYRIC_ROW_H)
                             : -1;
            if (tapped >= 0 && tapped < (int)g_lyrics.count)
            {
                detail_lyric_seek_to_index(tapped);
            }
        }
    }
    else if (code == LV_EVENT_PRESS_LOST)
    {
        g_detail.drag_active = false;
    }
}

lv_obj_t *ui_screen_now_playing_create(void)
{
    const lv_font_t *small = ui_font_zh_small();
    const lv_font_t *full = ui_font_zh_full();
    lv_obj_t *screen = lv_obj_create(NULL);
    ui_create_chrome(screen, "正在播放", UI_SCR_MUSIC, &g_detail.chrome);
    lv_obj_t *body = g_detail.chrome.body;
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(body, lv_color_hex(0x0C1723), 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_COVER, 0);

    lv_obj_t *back = detail_button(body, LV_SYMBOL_LEFT " 曲库", small,
                                   detail_back_cb, 92, -174, 10, NULL);
    lv_obj_set_height(back, 36);
    build_vinyl(body, &g_detail.vinyl, 252, 132);
    lv_obj_align(g_detail.vinyl.disc, LV_ALIGN_TOP_MID, 0, 54);
    g_detail.title = detail_label(body, full, UI_COL_TEXT, 416, LV_ALIGN_TOP_MID, 0, 319);
    lv_label_set_long_mode(g_detail.title, LV_LABEL_LONG_SCROLL_CIRCULAR);
    g_detail.artist = detail_label(body, small, UI_COL_MUTED, 408, LV_ALIGN_TOP_MID, 0, 352);

    lv_obj_t *lyrics = lv_obj_create(body);
    g_detail.lyrics = lyrics;
    lv_obj_add_style(lyrics, &g_st_card, 0);
    lv_obj_set_size(lyrics, 440, 174);
    lv_obj_align(lyrics, LV_ALIGN_TOP_MID, 0, 386);
    lv_obj_set_style_pad_all(lyrics, 0, 0);
    lv_obj_set_style_bg_color(lyrics, lv_color_hex(0x142230), 0);
    lv_obj_add_flag(lyrics, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(lyrics, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(lyrics, detail_lyrics_touch_cb, LV_EVENT_ALL, NULL);

    g_detail.lyric_focus_bar = lv_obj_create(lyrics);
    lv_obj_remove_style_all(g_detail.lyric_focus_bar);
    lv_obj_remove_flag(g_detail.lyric_focus_bar,
                       LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_set_size(g_detail.lyric_focus_bar, 420, LYRIC_ROW_H);
    lv_obj_align(g_detail.lyric_focus_bar, LV_ALIGN_CENTER, 0, -1);
    lv_obj_set_style_radius(g_detail.lyric_focus_bar, 8, 0);
    lv_obj_set_style_bg_color(g_detail.lyric_focus_bar, lv_color_hex(0x1A3148), 0);
    lv_obj_set_style_bg_opa(g_detail.lyric_focus_bar, LV_OPA_70, 0);
    lv_obj_set_style_border_color(g_detail.lyric_focus_bar, UI_COL_CYAN, 0);
    lv_obj_set_style_border_width(g_detail.lyric_focus_bar, 1, 0);
    lv_obj_set_style_border_opa(g_detail.lyric_focus_bar, LV_OPA_50, 0);
    lv_obj_add_flag(g_detail.lyric_focus_bar, LV_OBJ_FLAG_HIDDEN);

    g_detail.lyric_seek_time = lv_label_create(g_detail.lyric_focus_bar);
    lv_obj_remove_flag(g_detail.lyric_seek_time, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_set_style_text_font(g_detail.lyric_seek_time, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(g_detail.lyric_seek_time, UI_COL_CYAN, 0);
    lv_label_set_text(g_detail.lyric_seek_time, "00:00");
    lv_obj_align(g_detail.lyric_seek_time, LV_ALIGN_RIGHT_MID, -10, 0);

    g_detail.lyric_strip = lv_obj_create(lyrics);
    lv_obj_remove_style_all(g_detail.lyric_strip);
    lv_obj_remove_flag(g_detail.lyric_strip,
                       LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_set_size(g_detail.lyric_strip, 436, 166);
    lv_obj_align(g_detail.lyric_strip, LV_ALIGN_CENTER, 0, 0);
    for (unsigned i = 0; i < LYRIC_SLOTS; i++)
    {
        g_detail.lyric_labels[i] = detail_label(g_detail.lyric_strip, small, UI_COL_MUTED,
                                                404, LV_ALIGN_TOP_MID, 0,
                                                LYRIC_CENTER_Y + ((int)i - 3) * LYRIC_ROW_H);
        lv_obj_remove_flag(g_detail.lyric_labels[i],
                           LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
        g_detail.lyric_slot_role[i] = 0xFFU;
    }

    lv_obj_t *controls = lv_obj_create(body);
    lv_obj_remove_style_all(controls);
    lv_obj_remove_flag(controls, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(controls, 440, 98);
    lv_obj_align(controls, LV_ALIGN_BOTTOM_MID, 0, 0);

    g_detail.slider = lv_slider_create(controls);
    lv_obj_set_size(g_detail.slider, 416, 10);
    lv_obj_align(g_detail.slider, LV_ALIGN_TOP_MID, 0, 10);
    lv_slider_set_range(g_detail.slider, 0, 1000);
    lv_obj_set_ext_click_area(g_detail.slider, 10);
    lv_obj_set_style_bg_color(g_detail.slider, UI_COL_BORDER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_detail.slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(g_detail.slider, UI_COL_CYAN, LV_PART_INDICATOR);
    lv_obj_add_event_cb(g_detail.slider, music_seek_cb, LV_EVENT_RELEASED, NULL);
    g_detail.elapsed = detail_label(controls, &lv_font_montserrat_12, UI_COL_MUTED, 65,
                                    LV_ALIGN_TOP_LEFT, 10, 28);
    g_detail.duration = detail_label(controls, &lv_font_montserrat_12, UI_COL_MUTED, 65,
                                     LV_ALIGN_TOP_RIGHT, -10, 28);

    detail_button(controls, LV_SYMBOL_PREV, &lv_font_montserrat_20, music_prev_cb, 80, -122, 54, NULL);
    g_detail.play = detail_button(controls, LV_SYMBOL_PLAY, &lv_font_montserrat_24,
                                  music_play_cb, 110, 0, 54, &g_detail.play_symbol);
    lv_obj_set_style_bg_color(g_detail.play, UI_COL_CYAN, 0);
    detail_button(controls, LV_SYMBOL_NEXT, &lv_font_montserrat_20, music_next_cb, 80, 122, 54, NULL);
    lv_obj_set_style_text_color(g_detail.play_symbol, lv_color_hex(0x081422), 0);
    g_detail.lyric_index = INT32_MIN;
    detail_show_lyrics(false);
    return screen;
}

static void detail_lyrics_update(uint32_t elapsed_ms)
{
    bool have_lyrics = g_lyrics.count > 0U;
    detail_show_lyrics(have_lyrics);
    if (!have_lyrics) return;

    int key = ui_music_lyrics_find(&g_lyrics, elapsed_ms);
    if (g_detail.seek_hold_until != 0U)
    {
        if (key == g_detail.lyric_index || (int32_t)(lv_tick_get() - g_detail.seek_hold_until) >= 0)
            g_detail.seek_hold_until = 0U;
        else
            key = g_detail.lyric_index;
    }

    if (g_detail.user_browsing && !g_detail.drag_active &&
        (int32_t)(lv_tick_get() - g_detail.browse_until_tick) >= 0)
    {
        g_detail.user_browsing = false;
        g_detail.lyric_index = key;
        detail_lyrics_anim_scroll_to((int32_t)(key >= 0 ? key : 0) * LYRIC_ROW_H, 240U);
        return;
    }

    int old = g_detail.lyric_index;
    if (key == old) return;

    g_detail.lyric_index = key;
    if (g_detail.user_browsing)
    {
        detail_lyrics_render();
    }
    else
    {
        int32_t target_y = (int32_t)(key >= 0 ? key : 0) * LYRIC_ROW_H;
        detail_lyrics_anim_scroll_to(target_y, old >= 0 ? 220U : 0U);
    }
}

static void detail_update(const music_status_t *st)
{
    const char *split = strstr(st->title, " - ");
    char artist[MUSIC_TITLE_MAX];
    if (split)
    {
        size_t n = (size_t)(split - st->title);
        memcpy(artist, st->title, n);
        artist[n] = '\0';
    }
    else snprintf(artist, sizeof(artist), "%s", st->format == MUSIC_FMT_MP3 ? "MP3" :
                  st->format == MUSIC_FMT_WAV ? "WAV" : "");
    ui_set_label_text(g_detail.title, split && split[3] ? split + 3 : st->title);
    ui_set_label_text(g_detail.artist, artist);
    char elapsed[16], duration[16];
    format_mmss(st->elapsed_sec, elapsed, sizeof(elapsed));
    format_mmss(st->duration_sec, duration, sizeof(duration));
    ui_set_label_text(g_detail.elapsed, elapsed);
    ui_set_label_text(g_detail.duration, duration);
    if (!lv_obj_has_state(g_detail.slider, LV_STATE_PRESSED))
        lv_slider_set_value(g_detail.slider, st->progress_permille, LV_ANIM_OFF);
    bool playing = st->state == MUSIC_STATE_PLAYING;
    const char *symbol = playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY;
    if (strcmp(lv_label_get_text(g_detail.play_symbol), symbol) != 0)
    {
        ui_set_label_text(g_detail.play_symbol, symbol);
        lv_obj_set_style_bg_color(g_detail.play, playing ? UI_COL_AMBER : UI_COL_CYAN, 0);
    }
}

void ui_screen_music_on_enter(void)
{
    if (g_last_sd_mounted && g_last_sd_gen == sdcard_get_mount_gen() &&
        sdcard_is_mounted() && music_player_get_track_count() == 0U)
        (void)music_player_scan();
    g_last_rev = 0xFFFFFFFFU;
    ui_screen_music_update();
    vinyl_frame(ui_get_active_screen() == UI_SCR_NOW_PLAYING ? &g_detail.vinyl : &g_library_vinyl);
}

void ui_screen_music_update(void)
{
    music_status_t st;
    bool detail = ui_get_active_screen() == UI_SCR_NOW_PLAYING;
    ui_update_chrome(detail ? &g_detail.chrome : &g_chrome);

    uint32_t sd_gen = sdcard_get_mount_gen();
    bool mounted = sdcard_is_mounted();
    if (sd_gen != g_last_sd_gen || mounted != g_last_sd_mounted)
    {
        g_last_sd_gen = sd_gen;
        g_last_sd_mounted = mounted;
        (void)music_player_scan();
    }

    music_player_get_status(&st);

    bool library_changed = st.library_rev != g_last_library_rev;
    bool track_changed = st.current_index != g_last_active_idx;
    if (library_changed || track_changed)
        request_album_art(&st, sd_gen);

    if (library_changed)
    {
        rebuild_track_list(&st);
        g_last_library_rev = st.library_rev;
    }
    else if (track_changed)
    {
        refresh_active_row_highlight(st.current_index, st.track_count);
    }

    if (g_media_pending)
    {
        music_cover_state_t cover = ui_music_cover_copy(g_album_pixels, &g_lyrics);
        g_media_pending = cover == MUSIC_COVER_LOADING;
        if (!g_media_pending)
        {
            lv_anim_delete(&g_detail, lyric_scroll_anim_cb);
            g_detail.lyric_index = INT32_MIN;
            g_detail.scroll_y_px = 0;
            g_detail.last_rendered_y = INT32_MIN;
            g_detail.user_browsing = false;
            g_detail.drag_active = false;
            g_detail.seek_hold_until = 0U;
        }
        if (cover == MUSIC_COVER_READY)
        {
            sd_asset_dcache_clean(g_album_pixels, sizeof(g_album_pixels));
            lv_image_cache_drop(&g_album_image);
            vinyl_set_art(&g_library_vinyl, true);
            vinyl_set_art(&g_detail.vinyl, true);
            vinyl_frame(detail ? &g_detail.vinyl : &g_library_vinyl);
        }
    }

    if (detail)
    {
        rt_base_t level = rt_hw_interrupt_disable();
        uint8_t lreq = g_lyric_req;
        int32_t larg = g_lyric_req_arg;
        g_lyric_req = LYRIC_REQ_NONE;
        rt_hw_interrupt_enable(level);

        if (lreq == LYRIC_REQ_SCROLL && g_lyrics.count > 0U)
        {
            int base = g_detail.user_browsing
                           ? (int)((g_detail.scroll_y_px + LYRIC_ROW_H / 2) / LYRIC_ROW_H)
                           : (g_detail.lyric_index >= 0 ? g_detail.lyric_index : 0);
            int target = base + (int)larg;
            if (target < 0) target = 0;
            if (target >= (int)g_lyrics.count) target = (int)g_lyrics.count - 1;
            lv_anim_delete(&g_detail, lyric_scroll_anim_cb);
            g_detail.user_browsing = true;
            g_detail.browse_until_tick = lv_tick_get() + 6000U;
            g_detail.scroll_y_px = (int32_t)target * LYRIC_ROW_H;
            detail_lyrics_render();
            rt_kprintf("[I/music.ui] lyric scroll -> [%d] %lu ms: %s\n",
                       target, (unsigned long)g_lyrics.lines[target].time_ms,
                       g_lyrics.lines[target].text);
        }
        else if (lreq == LYRIC_REQ_TAP && g_lyrics.count > 0U)
        {
            int target = (larg >= 0) ? (int)larg
                                     : (int)((g_detail.scroll_y_px + LYRIC_ROW_H / 2) / LYRIC_ROW_H);
            if (target < 0) target = 0;
            if (target >= (int)g_lyrics.count) target = (int)g_lyrics.count - 1;
            detail_lyric_seek_to_index(target);
        }
    }

    g_vinyl_playing = st.sd_mounted && st.state == MUSIC_STATE_PLAYING && st.current_index >= 0;
    if (detail) detail_lyrics_update(st.elapsed_ms);
    if (st.rev == g_last_rev)
    {
        return;
    }
    g_last_rev = st.rev;
    if (detail)
    {
        detail_update(&st);
        return;
    }

    ui_set_label_text(g_lbl_title, st.title);

    const char *state_zh = (st.state == MUSIC_STATE_PLAYING) ? "播放中"
                         : ((st.state == MUSIC_STATE_PAUSED) ? "已暂停" : "已停止");
    const char *fmt_str  = (st.format == MUSIC_FMT_MP3) ? "MP3"
                         : ((st.format == MUSIC_FMT_WAV) ? "WAV" : "TF卡");
    char meta[96];
    if (st.sample_rate > 0U)
    {
        rt_snprintf(meta, sizeof(meta), "%s · %lu.%lukHz · %s",
                    fmt_str,
                    (unsigned long)(st.sample_rate / 1000U),
                    (unsigned long)((st.sample_rate % 1000U) / 100U),
                    state_zh);
    }
    else
    {
        rt_snprintf(meta, sizeof(meta), "%s · %s · %s",
                    fmt_str, state_zh, music_player_mode_name(st.mode));
    }
    ui_set_label_text(g_lbl_meta, meta);

    if (!lv_obj_has_state(g_slider_prog, LV_STATE_PRESSED))
    {
        lv_slider_set_value(g_slider_prog, (int32_t)st.progress_permille, LV_ANIM_OFF);
    }

    char t1[16], t2[16];
    format_mmss(st.elapsed_sec, t1, sizeof(t1));
    format_mmss(st.duration_sec, t2, sizeof(t2));
    ui_set_label_text(g_lbl_elapsed, t1);
    ui_set_label_text(g_lbl_duration, t2);

    if (st.state == MUSIC_STATE_PLAYING)
    {
        ui_set_label_text(g_lbl_play, LV_SYMBOL_PAUSE " 暂停");
        lv_obj_set_style_bg_color(g_btn_play, UI_COL_AMBER, 0);
    }
    else
    {
        ui_set_label_text(g_lbl_play, LV_SYMBOL_PLAY " 播放");
        lv_obj_set_style_bg_color(g_btn_play, UI_COL_CYAN, 0);
    }

    for (unsigned i = 0U; i < MUSIC_MODE_COUNT; i++)
    {
        bool sel = ((music_play_mode_t)i == st.mode);
        lv_obj_set_style_bg_color(g_mode_btns[i], sel ? lv_color_hex(0x163459) : UI_COL_CARD_HI, 0);
        lv_obj_set_style_border_color(g_mode_btns[i], sel ? UI_COL_CYAN : UI_COL_BORDER, 0);
        lv_obj_set_style_text_color(g_mode_lbls[i], sel ? UI_COL_CYAN : UI_COL_MUTED, 0);
    }
}

static int cmd_music_lyric(int argc, char **argv)
{
    if (argc < 2)
    {
        rt_kprintf("Usage: music_lyric <status|scroll [delta]|tap [idx]>\n");
        return -RT_EINVAL;
    }
    if (strcmp(argv[1], "status") == 0)
    {
        rt_kprintf("lyrics count=%u active=%d scroll_y=%ld browsing=%d\n",
                   g_lyrics.count, g_detail.lyric_index,
                   (long)g_detail.scroll_y_px, g_detail.user_browsing ? 1 : 0);
        return RT_EOK;
    }
    if (strcmp(argv[1], "scroll") == 0)
    {
        int32_t delta = (argc >= 3) ? (int32_t)atoi(argv[2]) : 5;
        rt_base_t level = rt_hw_interrupt_disable();
        g_lyric_req_arg = delta;
        g_lyric_req = LYRIC_REQ_SCROLL;
        rt_hw_interrupt_enable(level);
        return RT_EOK;
    }
    if (strcmp(argv[1], "tap") == 0)
    {
        int32_t idx = (argc >= 3) ? (int32_t)atoi(argv[2]) : -1;
        rt_base_t level = rt_hw_interrupt_disable();
        g_lyric_req_arg = idx;
        g_lyric_req = LYRIC_REQ_TAP;
        rt_hw_interrupt_enable(level);
        return RT_EOK;
    }
    rt_kprintf("Usage: music_lyric <status|scroll [delta]|tap [idx]>\n");
    return -RT_EINVAL;
}
MSH_CMD_EXPORT_ALIAS(cmd_music_lyric, music_lyric, Test lyrics scroll and tap-to-seek);
