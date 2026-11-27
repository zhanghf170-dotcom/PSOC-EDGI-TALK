/*
 * Gemini-chan (Gemini娘) — XiaoZhi AI Voice Companion LVGL Screen (480x800)
 *
 * Features:
 *   - 8 bespoke Gemini-chan anime expressions decoded via TJpgDec (RGB565) with D-Cache clean
 *   - Cosmic reactive halo & 5-bar live audio equalizer synced to mic/speaker RMS level
 *   - Live STT (user) & TTS (Gemini-chan) subtitles rendered with GB2312 Level-1 TinyTTF
 *   - xiaozhi.me 6-digit binding code banner when device activation is pending
 *   - Tap-to-talk (button or avatar), continuous multi-turn toggle, reconnect & expression demo
 */

#include <rtthread.h>
#include <stdio.h>
#include <string.h>
#include "board.h"
#include "ui_theme.h"
#include "ui_manager.h"
#include "xiaozhi.h"
#include "sd_assets.h"
#include "tjpgd.h"

#define GEMINI_IMG_W        320U
#define GEMINI_IMG_H        320U
#define GEMINI_IMG_BYTES    (GEMINI_IMG_W * GEMINI_IMG_H * 2U)
#define GEMINI_WORKBUF_SIZE 4096U
#define GEMINI_EQ_BARS      5U
#define GEMINI_JPG_MAX_SIZE (128U * 1024U)

typedef struct
{
    const uint8_t *data;
    size_t         size;
    size_t         offset;
    uint16_t      *dst;
} gemini_jpg_ctx_t;

static ui_chrome_t g_chrome;
static lv_obj_t *g_pill_status;
static lv_obj_t *g_dot_status;
static lv_obj_t *g_lbl_status;
static lv_obj_t *g_lbl_mood;
static lv_obj_t *g_stage_card;
static lv_obj_t *g_avatar_ring;
static lv_obj_t *g_avatar_img;
static lv_obj_t *g_eq_bars[GEMINI_EQ_BARS];
static lv_obj_t *g_act_banner;
static lv_obj_t *g_lbl_act_code;
static lv_obj_t *g_lbl_user;
static lv_obj_t *g_lbl_assistant;
static lv_obj_t *g_btn_talk;
static lv_obj_t *g_lbl_talk;
static lv_obj_t *g_btn_voice;
static lv_obj_t *g_lbl_voice;
static lv_obj_t *g_lbl_multi;

static uint16_t *g_avatar_pixels = RT_NULL;
static uint8_t  *g_jpg_workbuf   = RT_NULL;
static int       g_rendered_emo  = -1;
static uint32_t  g_last_ui_rev   = 0xFFFFFFFFU;
static uint32_t  g_anim_phase    = 0U;

static lv_image_dsc_t g_avatar_dsc = {
    .header = {
        .magic = LV_IMAGE_HEADER_MAGIC,
        .cf    = LV_COLOR_FORMAT_RGB565,
        .flags = LV_IMAGE_FLAGS_MODIFIABLE,
        .w     = GEMINI_IMG_W,
        .h     = GEMINI_IMG_H,
        .stride = GEMINI_IMG_W * 2U,
        .reserved_2 = 0,
    },
    .data_size = GEMINI_IMG_BYTES,
    .data      = NULL,
    .reserved  = NULL,
};

static const char *gemini_emotion_zh(xz_emotion_t emo)
{
    switch (emo)
    {
    case XZ_EMO_LISTENING: return "✦ 双子星 · 聆听";
    case XZ_EMO_THINKING:  return "✦ 双子星 · 思考";
    case XZ_EMO_SPEAKING:  return "✦ 双子星 · 开心";
    case XZ_EMO_WINKING:   return "✦ 双子星 · 俏皮";
    case XZ_EMO_SURPRISED: return "✦ 双子星 · 惊讶";
    case XZ_EMO_SAD:       return "✦ 双子星 · 委屈";
    case XZ_EMO_SLEEPY:    return "✦ 双子星 · 休眠";
    default:               return "✦ 双子星 · 待命";
    }
}

static const char *gemini_jpg_path(xz_emotion_t emo)
{
    switch (emo)
    {
    case XZ_EMO_LISTENING: return SD_GEMINI_DIR "/gemini_listening.jpg";
    case XZ_EMO_THINKING:  return SD_GEMINI_DIR "/gemini_thinking.jpg";
    case XZ_EMO_SPEAKING:  return SD_GEMINI_DIR "/gemini_speaking.jpg";
    case XZ_EMO_WINKING:   return SD_GEMINI_DIR "/gemini_winking.jpg";
    case XZ_EMO_SURPRISED: return SD_GEMINI_DIR "/gemini_surprised.jpg";
    case XZ_EMO_SAD:       return SD_GEMINI_DIR "/gemini_sad.jpg";
    case XZ_EMO_SLEEPY:    return SD_GEMINI_DIR "/gemini_sleepy.jpg";
    default:               return SD_GEMINI_DIR "/gemini_idle.jpg";
    }
}

static size_t gemini_jpg_input_cb(JDEC *jd, uint8_t *buf, size_t ndata)
{
    gemini_jpg_ctx_t *ctx = (gemini_jpg_ctx_t *)jd->device;
    size_t remain = (ctx->offset < ctx->size) ? (ctx->size - ctx->offset) : 0U;
    size_t take = (ndata < remain) ? ndata : remain;
    if (take > 0U)
    {
        if (buf != NULL)
        {
            rt_memcpy(buf, ctx->data + ctx->offset, take);
        }
        ctx->offset += take;
    }
    return take;
}

static int gemini_jpg_output_cb(JDEC *jd, void *bitmap, JRECT *rect)
{
    gemini_jpg_ctx_t *ctx = (gemini_jpg_ctx_t *)jd->device;
    const uint16_t *src = (const uint16_t *)bitmap;
    uint32_t rw = (uint32_t)(rect->right - rect->left + 1U);

    for (uint32_t y = rect->top; y <= rect->bottom; y++)
    {
        if (y < GEMINI_IMG_H)
        {
            uint32_t copy_w = rw;
            if (rect->left + copy_w > GEMINI_IMG_W)
            {
                copy_w = (rect->left < GEMINI_IMG_W) ? (GEMINI_IMG_W - rect->left) : 0U;
            }
            if (copy_w > 0U)
            {
                rt_memcpy(&ctx->dst[y * GEMINI_IMG_W + rect->left], src, copy_w * sizeof(uint16_t));
            }
        }
        src += rw;
    }
    return 1;
}

static void gemini_render_expression(xz_emotion_t emo)
{
    if ((int)emo == g_rendered_emo && g_avatar_pixels != RT_NULL)
    {
        return;
    }

    if (!sdcard_is_mounted())
    {
        return;
    }

    if (g_avatar_pixels == RT_NULL)
    {
        g_avatar_pixels = (uint16_t *)rt_malloc_align(GEMINI_IMG_BYTES, 64);
        if (g_avatar_pixels == RT_NULL)
        {
            return;
        }
        g_avatar_dsc.data = (const uint8_t *)g_avatar_pixels;
    }

    if (g_jpg_workbuf == RT_NULL)
    {
        g_jpg_workbuf = (uint8_t *)rt_malloc_align(GEMINI_WORKBUF_SIZE, 16);
        if (g_jpg_workbuf == RT_NULL)
        {
            return;
        }
    }

    const char *path = gemini_jpg_path(emo);
    FILE *fp = fopen(path, "rb");
    if (fp == NULL)
    {
        return;
    }
    if (fseek(fp, 0, SEEK_END) != 0)
    {
        fclose(fp);
        return;
    }
    long fsz = ftell(fp);
    if (fsz <= 0 || (size_t)fsz > GEMINI_JPG_MAX_SIZE || fseek(fp, 0, SEEK_SET) != 0)
    {
        fclose(fp);
        return;
    }

    uint8_t *jpg_data = (uint8_t *)rt_malloc((size_t)fsz);
    if (jpg_data == RT_NULL)
    {
        fclose(fp);
        return;
    }
    size_t nread = fread(jpg_data, 1U, (size_t)fsz, fp);
    fclose(fp);
    if (nread != (size_t)fsz)
    {
        rt_free(jpg_data);
        return;
    }

    gemini_jpg_ctx_t ctx = {
        .data   = jpg_data,
        .size   = nread,
        .offset = 0U,
        .dst    = g_avatar_pixels,
    };

    JDEC jd;
    if (jd_prepare(&jd, gemini_jpg_input_cb, g_jpg_workbuf, GEMINI_WORKBUF_SIZE, &ctx) == JDR_OK)
    {
        if (jd_decomp(&jd, gemini_jpg_output_cb, 0) == JDR_OK)
        {
            sd_asset_dcache_clean(g_avatar_pixels, GEMINI_IMG_BYTES);
            g_rendered_emo = (int)emo;
            lv_image_cache_drop(&g_avatar_dsc);
            lv_image_set_src(g_avatar_img, &g_avatar_dsc);
            lv_image_set_scale(g_avatar_img, LV_SCALE_NONE);
            lv_obj_invalidate(g_avatar_img);
        }
    }
    rt_free(jpg_data);
}

/* ---------------- Event Callbacks ---------------- */

static void talk_btn_cb(lv_event_t *e)
{
    (void)e;
    xiaozhi_trigger_talk();
}

static void voice_mode_btn_cb(lv_event_t *e)
{
    (void)e;
    xz_voice_mode_t cur = xiaozhi_get_voice_mode();
    xiaozhi_set_voice_mode(cur == XZ_VOICE_MODE_XIAOYI ? XZ_VOICE_MODE_CLOUD : XZ_VOICE_MODE_XIAOYI);
}

static void multi_turn_btn_cb(lv_event_t *e)
{
    (void)e;
    xiaozhi_set_multi_turn(!xiaozhi_get_multi_turn());
}

static void reconnect_btn_cb(lv_event_t *e)
{
    (void)e;
    xiaozhi_reconnect();
}

static void demo_emo_btn_cb(lv_event_t *e)
{
    (void)e;
    xiaozhi_cycle_demo_emotion();
}

/* ---------------- Screen Creation ---------------- */

lv_obj_t *ui_screen_xiaozhi_create(void)
{
    const lv_font_t *f_full  = ui_font_zh_full();
    const lv_font_t *f_small = ui_font_zh_small();

    lv_obj_t *scr = lv_obj_create(NULL);
    ui_create_chrome(scr, "Gemini娘 · 小智AI", UI_SCR_XIAOZHI, &g_chrome);

    lv_obj_t *body = g_chrome.body;
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(body, 8, 0);
    lv_obj_set_style_pad_all(body, 10, 0);

    /* 1. Status & Emotion Pill Row (460 x 34) */
    lv_obj_t *top_row = lv_obj_create(body);
    lv_obj_remove_style_all(top_row);
    lv_obj_set_size(top_row, 460, 34);
    lv_obj_remove_flag(top_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(top_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    g_pill_status = lv_obj_create(top_row);
    lv_obj_add_style(g_pill_status, &g_st_pill, 0);
    lv_obj_set_size(g_pill_status, LV_SIZE_CONTENT, 32);
    lv_obj_set_style_pad_hor(g_pill_status, 12, 0);
    lv_obj_set_style_pad_column(g_pill_status, 8, 0);
    lv_obj_set_flex_flow(g_pill_status, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(g_pill_status, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(g_pill_status, LV_OBJ_FLAG_SCROLLABLE);

    g_dot_status = lv_obj_create(g_pill_status);
    lv_obj_remove_style_all(g_dot_status);
    lv_obj_set_size(g_dot_status, 10, 10);
    lv_obj_set_style_radius(g_dot_status, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(g_dot_status, UI_COL_GEMINI_PURPLE, 0);
    lv_obj_set_style_bg_opa(g_dot_status, LV_OPA_COVER, 0);

    g_lbl_status = lv_label_create(g_pill_status);
    lv_label_set_text(g_lbl_status, "等待联网");
    lv_obj_set_style_text_font(g_lbl_status, f_small, 0);
    lv_obj_set_style_text_color(g_lbl_status, UI_COL_TEXT, 0);

    lv_obj_t *mood_pill = lv_obj_create(top_row);
    lv_obj_add_style(mood_pill, &g_st_pill, 0);
    lv_obj_set_size(mood_pill, LV_SIZE_CONTENT, 32);
    lv_obj_set_style_border_color(mood_pill, UI_COL_GEMINI_PURPLE, 0);
    lv_obj_remove_flag(mood_pill, LV_OBJ_FLAG_SCROLLABLE);

    g_lbl_mood = lv_label_create(mood_pill);
    lv_label_set_text(g_lbl_mood, "✦ 双子星 · 待命");
    lv_obj_set_style_text_font(g_lbl_mood, f_small, 0);
    lv_obj_set_style_text_color(g_lbl_mood, UI_COL_GEMINI_PURPLE, 0);

    /* 2. Gemini-chan Cosmic Portrait Stage (460 x 342) */
    g_stage_card = lv_obj_create(body);
    lv_obj_set_size(g_stage_card, 460, 342);
    lv_obj_set_style_bg_color(g_stage_card, UI_COL_BG, 0);
    lv_obj_set_style_bg_opa(g_stage_card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(g_stage_card, lv_color_hex(0x2E2A58), 0);
    lv_obj_set_style_border_width(g_stage_card, 2, 0);
    lv_obj_set_style_radius(g_stage_card, 20, 0);
    lv_obj_set_style_pad_all(g_stage_card, 0, 0);
    lv_obj_remove_flag(g_stage_card, LV_OBJ_FLAG_SCROLLABLE);

    /* Glowing cosmic frame around 320x320 portrait */
    g_avatar_ring = lv_obj_create(g_stage_card);
    lv_obj_set_size(g_avatar_ring, 328, 328);
    lv_obj_center(g_avatar_ring);
    lv_obj_set_style_bg_color(g_avatar_ring, UI_COL_BG, 0);
    lv_obj_set_style_bg_opa(g_avatar_ring, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(g_avatar_ring, UI_COL_GEMINI_PURPLE, 0);
    lv_obj_set_style_border_width(g_avatar_ring, 3, 0);
    lv_obj_set_style_radius(g_avatar_ring, 26, 0);
    lv_obj_set_style_pad_all(g_avatar_ring, 0, 0);
    lv_obj_set_style_clip_corner(g_avatar_ring, true, 0);
    lv_obj_remove_flag(g_avatar_ring, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_avatar_ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(g_avatar_ring, talk_btn_cb, LV_EVENT_CLICKED, NULL);

    g_avatar_img = lv_image_create(g_avatar_ring);
    lv_image_set_src(g_avatar_img, &g_ui_gemini_icon_48);
    lv_image_set_scale(g_avatar_img, 768); /* 48 * 3 = 144x144 built-in placeholder */
    lv_obj_center(g_avatar_img);
    lv_obj_remove_flag(g_avatar_img, LV_OBJ_FLAG_CLICKABLE);

    /* Floating 5-bar cosmic audio visualizer overlay along bottom of stage */
    lv_obj_t *eq_box = lv_obj_create(g_stage_card);
    lv_obj_remove_style_all(eq_box);
    lv_obj_set_size(eq_box, 120, 28);
    lv_obj_align(eq_box, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_flex_flow(eq_box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(eq_box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(eq_box, 6, 0);
    lv_obj_remove_flag(eq_box, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    static const uint32_t eq_colors[GEMINI_EQ_BARS] = {
        0x38BDF8, 0x60A5FA, 0xA78BFA, 0xF472B6, 0xFBBF24
    };
    for (unsigned i = 0; i < GEMINI_EQ_BARS; i++)
    {
        g_eq_bars[i] = lv_obj_create(eq_box);
        lv_obj_remove_style_all(g_eq_bars[i]);
        lv_obj_set_size(g_eq_bars[i], 10, 4);
        lv_obj_set_style_radius(g_eq_bars[i], 5, 0);
        lv_obj_set_style_bg_color(g_eq_bars[i], lv_color_hex(eq_colors[i]), 0);
        lv_obj_set_style_bg_opa(g_eq_bars[i], LV_OPA_80, 0);
    }

    /* 3. Live Dialogue & Activation Card (460 x 172) */
    lv_obj_t *chat_card = lv_obj_create(body);
    lv_obj_add_style(chat_card, &g_st_card, 0);
    lv_obj_set_size(chat_card, 460, 172);
    lv_obj_set_style_border_color(chat_card, lv_color_hex(0x2D3A5C), 0);
    lv_obj_set_style_pad_hor(chat_card, 12, 0);
    lv_obj_set_style_pad_ver(chat_card, 8, 0);
    lv_obj_set_style_pad_row(chat_card, 5, 0);
    lv_obj_set_flex_flow(chat_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(chat_card, LV_SCROLLBAR_MODE_AUTO);

    /* Activation Code Banner (fixed non-shrinking height so text never clips) */
    g_act_banner = lv_obj_create(chat_card);
    lv_obj_set_size(g_act_banner, 434, 30);
    lv_obj_set_style_min_height(g_act_banner, 30, 0);
    lv_obj_set_style_max_height(g_act_banner, 30, 0);
    lv_obj_set_flex_grow(g_act_banner, 0);
    lv_obj_set_style_bg_color(g_act_banner, lv_color_hex(0x2E1F5E), 0);
    lv_obj_set_style_bg_opa(g_act_banner, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(g_act_banner, UI_COL_GEMINI_GOLD, 0);
    lv_obj_set_style_border_width(g_act_banner, 1, 0);
    lv_obj_set_style_radius(g_act_banner, 8, 0);
    lv_obj_set_style_pad_all(g_act_banner, 0, 0);
    lv_obj_remove_flag(g_act_banner, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_act_banner, LV_OBJ_FLAG_HIDDEN);

    g_lbl_act_code = lv_label_create(g_act_banner);
    lv_label_set_text(g_lbl_act_code, "");
    lv_obj_set_style_text_font(g_lbl_act_code, f_small, 0);
    lv_obj_set_style_text_color(g_lbl_act_code, UI_COL_GEMINI_GOLD, 0);
    lv_obj_center(g_lbl_act_code);

    g_lbl_user = lv_label_create(chat_card);
    lv_obj_set_width(g_lbl_user, 434);
    lv_label_set_long_mode(g_lbl_user, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(g_lbl_user, f_small, 0);
    lv_obj_set_style_text_color(g_lbl_user, UI_COL_GEMINI_BLUE, 0);
    lv_label_set_text(g_lbl_user, "我：按 SW3(P8.3) 或点击头像/按钮开始语音对话");

    g_lbl_assistant = lv_label_create(chat_card);
    lv_obj_set_width(g_lbl_assistant, 434);
    lv_label_set_long_mode(g_lbl_assistant, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(g_lbl_assistant, f_full, 0);
    lv_obj_set_style_text_color(g_lbl_assistant, UI_COL_TEXT, 0);
    lv_label_set_text(g_lbl_assistant, "Gemini：主人好，我是双子星猫娘助手，随时准备为你护航喵。");

    /* 4. Voice Control Dock (460 x 96) */
    g_btn_talk = lv_button_create(body);
    lv_obj_set_size(g_btn_talk, 460, 50);
    lv_obj_set_style_radius(g_btn_talk, 14, 0);
    lv_obj_set_style_bg_color(g_btn_talk, UI_COL_GEMINI_VIOLET, 0);
    lv_obj_set_style_bg_grad_color(g_btn_talk, lv_color_hex(0x3B82F6), 0);
    lv_obj_set_style_bg_grad_dir(g_btn_talk, LV_GRAD_DIR_HOR, 0);
    lv_obj_set_style_bg_opa(g_btn_talk, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_btn_talk, 0, 0);
    lv_obj_set_style_shadow_width(g_btn_talk, 0, 0);
    lv_obj_add_event_cb(g_btn_talk, talk_btn_cb, LV_EVENT_CLICKED, NULL);

    g_lbl_talk = lv_label_create(g_btn_talk);
    lv_label_set_text(g_lbl_talk, LV_SYMBOL_AUDIO "  点击和 Gemini娘 说话");
    lv_obj_set_style_text_font(g_lbl_talk, f_full, 0);
    lv_obj_set_style_text_color(g_lbl_talk, UI_COL_TEXT, 0);
    lv_obj_center(g_lbl_talk);

    lv_obj_t *sub_row = lv_obj_create(body);
    lv_obj_remove_style_all(sub_row);
    lv_obj_set_size(sub_row, 460, 38);
    lv_obj_set_flex_flow(sub_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(sub_row, 6, 0);
    lv_obj_remove_flag(sub_row, LV_OBJ_FLAG_SCROLLABLE);

    g_btn_voice = lv_button_create(sub_row);
    lv_obj_add_style(g_btn_voice, &g_st_btn_ghost, 0);
    lv_obj_set_style_border_color(g_btn_voice, UI_COL_GEMINI_PURPLE, 0);
    lv_obj_set_flex_grow(g_btn_voice, 1);
    lv_obj_set_height(g_btn_voice, 36);
    lv_obj_add_event_cb(g_btn_voice, voice_mode_btn_cb, LV_EVENT_CLICKED, NULL);
    g_lbl_voice = lv_label_create(g_btn_voice);
    lv_label_set_text(g_lbl_voice, "云端音");
    lv_obj_set_style_text_font(g_lbl_voice, f_small, 0);
    lv_obj_set_style_text_color(g_lbl_voice, UI_COL_CYAN, 0);
    lv_obj_center(g_lbl_voice);

    lv_obj_t *b_multi = lv_button_create(sub_row);
    lv_obj_add_style(b_multi, &g_st_btn_ghost, 0);
    lv_obj_set_flex_grow(b_multi, 1);
    lv_obj_set_height(b_multi, 36);
    lv_obj_add_event_cb(b_multi, multi_turn_btn_cb, LV_EVENT_CLICKED, NULL);
    g_lbl_multi = lv_label_create(b_multi);
    lv_label_set_text(g_lbl_multi, "连续:开");
    lv_obj_set_style_text_font(g_lbl_multi, f_small, 0);
    lv_obj_center(g_lbl_multi);

    lv_obj_t *b_reconn = lv_button_create(sub_row);
    lv_obj_add_style(b_reconn, &g_st_btn_ghost, 0);
    lv_obj_set_flex_grow(b_reconn, 1);
    lv_obj_set_height(b_reconn, 36);
    lv_obj_add_event_cb(b_reconn, reconnect_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l_rec = lv_label_create(b_reconn);
    lv_label_set_text(l_rec, LV_SYMBOL_REFRESH " 重连");
    lv_obj_set_style_text_font(l_rec, f_small, 0);
    lv_obj_center(l_rec);

    lv_obj_t *b_emo = lv_button_create(sub_row);
    lv_obj_add_style(b_emo, &g_st_btn_ghost, 0);
    lv_obj_set_style_border_color(b_emo, UI_COL_GEMINI_PINK, 0);
    lv_obj_set_flex_grow(b_emo, 1);
    lv_obj_set_height(b_emo, 36);
    lv_obj_add_event_cb(b_emo, demo_emo_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l_emo = lv_label_create(b_emo);
    lv_label_set_text(l_emo, LV_SYMBOL_VOLUME_MAX " 听她说");
    lv_obj_set_style_text_font(l_emo, f_small, 0);
    lv_obj_set_style_text_color(l_emo, UI_COL_GEMINI_PINK, 0);
    lv_obj_center(l_emo);

    return scr;
}

void ui_screen_xiaozhi_on_enter(void)
{
    xz_ui_snapshot_t snap;
    xiaozhi_greet_once();
    xiaozhi_get_snapshot(&snap);
    gemini_render_expression(snap.emotion);
    g_last_ui_rev = 0xFFFFFFFFU;
    ui_screen_xiaozhi_update();
}

void ui_screen_xiaozhi_on_leave(void)
{
    if (g_avatar_img != NULL)
    {
        lv_image_set_src(g_avatar_img, &g_ui_gemini_icon_48);
        lv_image_set_scale(g_avatar_img, 768);
    }
    lv_image_cache_drop(&g_avatar_dsc);
    if (g_avatar_pixels != RT_NULL)
    {
        rt_free_align(g_avatar_pixels);
        g_avatar_pixels = RT_NULL;
        g_avatar_dsc.data = NULL;
    }
    if (g_jpg_workbuf != RT_NULL)
    {
        rt_free_align(g_jpg_workbuf);
        g_jpg_workbuf = RT_NULL;
    }
    g_rendered_emo = -1;
}

void ui_screen_xiaozhi_update(void)
{
    xz_ui_snapshot_t snap;
    ui_update_chrome(&g_chrome);
    xiaozhi_get_snapshot(&snap);

    /* Ensure initial portrait is decoded */
    if (g_rendered_emo != (int)snap.emotion)
    {
        gemini_render_expression(snap.emotion);
    }

    /* Update live cosmic equalizer bars & glow ring every tick (~60ms) */
    g_anim_phase++;
    uint8_t energy = 0U;
    if (snap.state == XZ_STATE_LISTENING)
        energy = snap.mic_level > 8U ? snap.mic_level : (uint8_t)(18U + ((g_anim_phase * 7U) % 24U));
    else if (snap.state == XZ_STATE_SPEAKING)
        energy = snap.spk_level > 8U ? snap.spk_level : (uint8_t)(25U + ((g_anim_phase * 11U) % 45U));
    else if (snap.state == XZ_STATE_THINKING || snap.state == XZ_STATE_CONNECTING)
        energy = (uint8_t)(16U + ((g_anim_phase * 5U) % 20U));

    for (unsigned i = 0; i < GEMINI_EQ_BARS; i++)
    {
        int32_t h = 4;
        if (energy > 0U)
        {
            uint32_t wave = ((g_anim_phase + i * 3U) % 8U);
            uint32_t factor = (wave < 4U) ? (wave + 2U) : (9U - wave);
            h = 5 + (int32_t)((energy * factor) / 28U);
            if (h > 26) h = 26;
        }
        lv_obj_set_height(g_eq_bars[i], h);
    }

    /* Only refresh text & styles when state revision changes */
    if (snap.ui_rev == g_last_ui_rev)
    {
        return;
    }
    g_last_ui_rev = snap.ui_rev;

    lv_color_t accent = UI_COL_GEMINI_PURPLE;
    const char *btn_txt = LV_SYMBOL_AUDIO "  点击和 Gemini娘 说话";
    switch (snap.state)
    {
    case XZ_STATE_LISTENING:
        accent  = UI_COL_CYAN;
        btn_txt = LV_SYMBOL_STOP "  正在聆听 · 点击发送";
        break;
    case XZ_STATE_SPEAKING:
        accent  = UI_COL_GEMINI_PINK;
        btn_txt = LV_SYMBOL_VOLUME_MAX "  Gemini娘说话中 · 点击打断";
        break;
    case XZ_STATE_THINKING:
    case XZ_STATE_CONNECTING:
        accent  = UI_COL_GEMINI_GOLD;
        btn_txt = LV_SYMBOL_REFRESH "  双子星思考连接中...";
        break;
    case XZ_STATE_ACTIVATING:
        accent  = UI_COL_GEMINI_GOLD;
        btn_txt = LV_SYMBOL_OK "  已绑定点此对话";
        break;
    case XZ_STATE_IDLE:
        accent  = UI_COL_GREEN;
        btn_txt = LV_SYMBOL_AUDIO "  点击和 Gemini娘 对话";
        break;
    case XZ_STATE_ERROR:
        accent  = UI_COL_RED;
        btn_txt = LV_SYMBOL_REFRESH "  重新连接小智 AI";
        break;
    default:
        accent  = UI_COL_GEMINI_PURPLE;
        btn_txt = snap.wifi_ready ? LV_SYMBOL_AUDIO "  唤醒 Gemini娘" : LV_SYMBOL_AUDIO "  互动演示 / 离线模式";
        break;
    }

    lv_obj_set_style_bg_color(g_dot_status, accent, 0);
    lv_obj_set_style_border_color(g_avatar_ring, accent, 0);
    ui_set_label_text(g_lbl_status, snap.status_text[0] ? snap.status_text : "待命中");
    ui_set_label_text(g_lbl_mood, gemini_emotion_zh(snap.emotion));
    ui_set_label_text(g_lbl_talk, btn_txt);
    ui_set_label_text(g_lbl_multi, snap.multi_turn ? "连续:开" : "连续:关");
    if (g_lbl_voice != NULL)
    {
        bool is_xy = (snap.voice_mode == XZ_VOICE_MODE_XIAOYI);
        ui_set_label_text(g_lbl_voice, is_xy ? "萌妹音" : "云端音");
        lv_obj_set_style_text_color(g_lbl_voice, is_xy ? UI_COL_GEMINI_PURPLE : UI_COL_CYAN, 0);
        lv_obj_set_style_border_color(g_btn_voice, is_xy ? UI_COL_GEMINI_PURPLE : UI_COL_BORDER, 0);
    }

    if (snap.activation_code[0] != '\0')
    {
        char act_buf[96];
        rt_snprintf(act_buf, sizeof(act_buf), "xiaozhi.me 绑定验证码: %s", snap.activation_code);
        ui_set_label_text(g_lbl_act_code, act_buf);
        lv_obj_remove_flag(g_act_banner, LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
        lv_obj_add_flag(g_act_banner, LV_OBJ_FLAG_HIDDEN);
    }

    if (snap.user_text[0] != '\0')
    {
        char ubuf[184];
        rt_snprintf(ubuf, sizeof(ubuf), "我：%s", snap.user_text);
        ui_set_label_text(g_lbl_user, ubuf);
    }

    if (snap.assistant_text[0] != '\0')
    {
        char abuf[288];
        rt_snprintf(abuf, sizeof(abuf), "Gemini：%s", snap.assistant_text);
        ui_set_label_text(g_lbl_assistant, abuf);
    }
}
