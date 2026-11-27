#include <rtthread.h>
#include <stdio.h>
#include <sys/stat.h>
#include "board.h"
#include "ui_manager.h"
#include "sd_assets.h"
#include "libs/gif/gifdec.h"

#define THUMB_W          192U
#define THUMB_H          224U
#define RGB565_BYTES(w, h) ((uint32_t)(w) * (uint32_t)(h) * 2U)
#define THUMB_BYTES      RGB565_BYTES(THUMB_W, THUMB_H)
#define ITEM_COUNT       4U
#define THUMB_TOTAL_BYTES (THUMB_BYTES * ITEM_COUNT)

typedef struct
{
    const char *name;
    bool        is_gif;
    uint16_t    w;
    uint16_t    h;
    uint32_t    photo_offset;
    uint32_t    photo_bytes;
} gallery_item_meta_t;

static const gallery_item_meta_t g_items[ITEM_COUNT] = {
    {"照片 1", false, 256U, 576U, 0U,                                                   RGB565_BYTES(256U, 576U)},
    {"照片 2", false, 448U, 320U, RGB565_BYTES(256U, 576U),                             RGB565_BYTES(448U, 320U)},
    {"照片 3", false, 448U, 528U, RGB565_BYTES(256U, 576U) + RGB565_BYTES(448U, 320U),  RGB565_BYTES(448U, 528U)},
    {"动图 1", true,  320U, 320U, 0U,                                                   0U},
};

static ui_chrome_t chrome;
static lv_obj_t *grid, *empty_card, *empty_lbl, *detail, *image_box, *detail_image, *caption;
static lv_obj_t *thumb_imgs[ITEM_COUNT];

static lv_image_dsc_t g_thumb_dsc[ITEM_COUNT];
static uint8_t       *g_thumbs_buf = RT_NULL;
static bool           g_thumbs_ready = false;
static uint32_t       g_gallery_sd_gen = 0U;
static uint32_t       g_gallery_tried_sd_gen = 0U;
static bool           g_gallery_sd_was_mounted = false;

static lv_image_dsc_t g_photo_dsc;
static uint8_t       *g_photo_buf = RT_NULL;

static gd_GIF        *g_gif = NULL;
static lv_timer_t    *g_gif_timer = NULL;
static lv_image_dsc_t g_gif_dsc;
static uint8_t       *g_gif_file_buf  = RT_NULL;
static uint8_t       *g_gif_frame_buf = RT_NULL;
static uint32_t       g_gif_last_tick = 0U;
static uint32_t       g_gif_frames = 0U;

static unsigned selected = 0U;

static void gallery_stop_gif(void)
{
    if (g_gif_timer != NULL)
    {
        lv_timer_pause(g_gif_timer);
    }
    if (detail_image != NULL)
    {
        lv_image_set_src(detail_image, NULL);
    }
    if (g_gif_dsc.data != NULL)
    {
        lv_image_cache_drop(&g_gif_dsc);
        g_gif_dsc.data = NULL;
    }
    if (g_gif != NULL)
    {
        gd_close_gif(g_gif);
        g_gif = NULL;
    }
    if (g_gif_frame_buf != RT_NULL)
    {
        rt_free_align(g_gif_frame_buf);
        g_gif_frame_buf = RT_NULL;
    }
    if (g_gif_file_buf != RT_NULL)
    {
        rt_free_align(g_gif_file_buf);
        g_gif_file_buf = RT_NULL;
    }
}

static void gallery_free_photo(void)
{
    if (detail_image != NULL)
    {
        lv_image_set_src(detail_image, NULL);
    }
    if (g_photo_buf != RT_NULL)
    {
        lv_image_cache_drop(&g_photo_dsc);
        rt_free_align(g_photo_buf);
        g_photo_buf = RT_NULL;
    }
    g_photo_dsc.data = NULL;
}

static void gallery_free_thumbs(void)
{
    for (unsigned i = 0U; i < ITEM_COUNT; i++)
    {
        if (thumb_imgs[i] != NULL)
        {
            lv_image_set_src(thumb_imgs[i], NULL);
        }
        if (g_thumb_dsc[i].data != NULL)
        {
            lv_image_cache_drop(&g_thumb_dsc[i]);
            g_thumb_dsc[i].data = NULL;
        }
    }
    if (g_thumbs_buf != RT_NULL)
    {
        rt_free_align(g_thumbs_buf);
        g_thumbs_buf = RT_NULL;
    }
    g_thumbs_ready = false;
}

static bool gallery_load_thumbs(void)
{
    uint32_t gen = sdcard_get_mount_gen();
    if (g_thumbs_ready && g_gallery_sd_gen == gen)
    {
        return true;
    }
    if (!sdcard_is_mounted())
    {
        return false;
    }

    /* Record attempt for this mount generation so ui_screen_gallery_update()
     * does not spin retrying every 250ms when assets are missing or unreadable. */
    g_gallery_tried_sd_gen = gen;
    gallery_free_thumbs();

    g_thumbs_buf = (uint8_t *)rt_malloc_align(THUMB_TOTAL_BYTES, 64);
    if (g_thumbs_buf == RT_NULL)
    {
        return false;
    }

    if (sd_asset_read_range(SD_GALLERY_THUMBS_PATH, 0U, g_thumbs_buf, THUMB_TOTAL_BYTES, RT_TRUE) != RT_EOK)
    {
        rt_free_align(g_thumbs_buf);
        g_thumbs_buf = RT_NULL;
        return false;
    }

    for (unsigned i = 0U; i < ITEM_COUNT; i++)
    {
        rt_memset(&g_thumb_dsc[i], 0, sizeof(g_thumb_dsc[i]));
        g_thumb_dsc[i].header.magic  = LV_IMAGE_HEADER_MAGIC;
        g_thumb_dsc[i].header.cf     = LV_COLOR_FORMAT_RGB565;
        g_thumb_dsc[i].header.flags  = LV_IMAGE_FLAGS_MODIFIABLE;
        g_thumb_dsc[i].header.w      = THUMB_W;
        g_thumb_dsc[i].header.h      = THUMB_H;
        g_thumb_dsc[i].header.stride = THUMB_W * 2U;
        g_thumb_dsc[i].data_size     = THUMB_BYTES;
        g_thumb_dsc[i].data          = g_thumbs_buf + (i * THUMB_BYTES);
        lv_image_cache_drop(&g_thumb_dsc[i]);
        if (thumb_imgs[i] != NULL)
        {
            lv_image_set_src(thumb_imgs[i], &g_thumb_dsc[i]);
        }
    }

    g_gallery_sd_gen = gen;
    g_thumbs_ready = true;
    return true;
}

static void gallery_sync_empty_state(void)
{
    bool ok = gallery_load_thumbs();
    if (ok)
    {
        if (empty_card != NULL) lv_obj_add_flag(empty_card, LV_OBJ_FLAG_HIDDEN);
        if (grid != NULL && (detail == NULL || lv_obj_has_flag(detail, LV_OBJ_FLAG_HIDDEN)))
        {
            lv_obj_remove_flag(grid, LV_OBJ_FLAG_HIDDEN);
        }
    }
    else
    {
        if (grid != NULL) lv_obj_add_flag(grid, LV_OBJ_FLAG_HIDDEN);
        if (empty_card != NULL) lv_obj_remove_flag(empty_card, LV_OBJ_FLAG_HIDDEN);
        if (empty_lbl != NULL)
        {
            ui_set_label_text(empty_lbl,
                              sdcard_is_mounted()
                                  ? "未找到 TF 卡图库资源\n请检查 /sdcard/gallery 目录"
                                  : "未检测到 TF 卡\n请插入 TF 卡后点击重新加载");
        }
    }
}

static void gif_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (g_gif == NULL || g_gif_dsc.data == NULL || g_gif_frame_buf == RT_NULL)
    {
        return;
    }

    uint32_t delay_ms = (uint32_t)g_gif->gce.delay * 10U;
    if (delay_ms < 20U)
    {
        delay_ms = 80U;
    }
    if (lv_tick_elaps(g_gif_last_tick) < delay_ms)
    {
        return;
    }
    g_gif_last_tick = lv_tick_get();

    int has_next = gd_get_frame(g_gif);
    if (has_next == 0)
    {
        gd_rewind(g_gif);
        has_next = gd_get_frame(g_gif);
    }
    if (has_next < 0)
    {
        gallery_stop_gif();
        if (caption != NULL)
        {
            lv_label_set_text(caption, "动图读取异常，请返回重试");
        }
        return;
    }
    if (has_next > 0)
    {
        gd_render_frame(g_gif, g_gif->canvas);
        rt_memcpy(g_gif_frame_buf, g_gif->canvas, g_gif_dsc.data_size);
        sd_asset_dcache_clean((void *)g_gif_frame_buf, g_gif_dsc.data_size);
        lv_image_cache_drop(&g_gif_dsc);
        lv_obj_invalidate(detail_image);
        g_gif_frames++;
    }
}

static int gallery_status(void)
{
    rt_kprintf("Gallery selected=%u items=%u sd_ok=%d anim=%d frames=%lu\n",
               selected, (unsigned)ITEM_COUNT,
               sdcard_is_mounted() ? 1 : 0,
               (g_gif != NULL) ? 1 : 0,
               (unsigned long)g_gif_frames);
    return 0;
}
MSH_CMD_EXPORT(gallery_status, Show gallery status);

void ui_screen_gallery_on_enter(void)
{
    g_gallery_tried_sd_gen = 0U;
    gallery_sync_empty_state();
}

void ui_screen_gallery_open(unsigned index)
{
    bool load_ok = false;

    if (index >= ITEM_COUNT)
        return;

    if (!gallery_load_thumbs())
    {
        gallery_sync_empty_state();
        return;
    }

    gallery_stop_gif();
    gallery_free_photo();
    selected = index;

    if (g_items[index].is_gif)
    {
        struct stat st;
        if (stat(SD_GALLERY_GIF_PATH, &st) == 0 && st.st_size > 0 &&
            (size_t)st.st_size <= (2U * 1024U * 1024U))
        {
            g_gif_file_buf = (uint8_t *)rt_malloc_align((size_t)st.st_size, 64);
            if (g_gif_file_buf != RT_NULL)
            {
                if (sd_asset_read_range(SD_GALLERY_GIF_PATH, 0U, g_gif_file_buf,
                                        (size_t)st.st_size, RT_FALSE) == RT_EOK)
                {
                    g_gif = gd_open_gif_data(g_gif_file_buf);
                }
                if (g_gif == NULL)
                {
                    rt_free_align(g_gif_file_buf);
                    g_gif_file_buf = RT_NULL;
                }
            }
        }
        if (g_gif == NULL)
        {
            g_gif = gd_open_gif_file(SD_GALLERY_GIF_LV_PATH);
        }
        if (g_gif != NULL)
        {
            uint32_t frame_bytes = (uint32_t)g_gif->width * (uint32_t)g_gif->height * 4U;
            g_gif_frame_buf = (uint8_t *)rt_malloc_align(frame_bytes, 64);
            if (g_gif_frame_buf == RT_NULL)
            {
                gallery_stop_gif();
            }
            else
            {
                rt_memset(&g_gif_dsc, 0, sizeof(g_gif_dsc));
                g_gif_dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
                g_gif_dsc.header.cf     = LV_COLOR_FORMAT_ARGB8888;
                g_gif_dsc.header.flags  = LV_IMAGE_FLAGS_MODIFIABLE;
                g_gif_dsc.header.w      = g_gif->width;
                g_gif_dsc.header.h      = g_gif->height;
                g_gif_dsc.header.stride = g_gif->width * 4U;
                g_gif_dsc.data_size     = frame_bytes;
                g_gif_dsc.data          = g_gif_frame_buf;

                g_gif_frames = 0U;
                g_gif_last_tick = 0U;
                if (gd_get_frame(g_gif) > 0)
                {
                    gd_render_frame(g_gif, g_gif->canvas);
                    rt_memcpy(g_gif_frame_buf, g_gif->canvas, frame_bytes);
                    sd_asset_dcache_clean((void *)g_gif_frame_buf, frame_bytes);
                    g_gif_frames = 1U;
                    load_ok = true;
                }
                else
                {
                    rt_memcpy(g_gif_frame_buf, g_gif->canvas, frame_bytes);
                    sd_asset_dcache_clean((void *)g_gif_frame_buf, frame_bytes);
                }

                if (load_ok)
                {
                    lv_image_cache_drop(&g_gif_dsc);
                    lv_obj_set_size(detail_image, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
                    lv_image_set_src(detail_image, &g_gif_dsc);
                    lv_image_set_pivot(detail_image, g_gif->width / 2, g_gif->height / 2);
                    lv_image_set_scale(detail_image, 320); /* 320 * 1.25 = 400x400 crisp center view */
                    lv_obj_center(detail_image);

                    if (g_gif_timer == NULL)
                    {
                        g_gif_timer = lv_timer_create(gif_timer_cb, 20, NULL);
                    }
                    lv_timer_resume(g_gif_timer);
                    lv_timer_reset(g_gif_timer);
                }
                else
                {
                    gallery_stop_gif();
                }
            }
        }
    }
    else
    {
        uint32_t bytes = g_items[index].photo_bytes;
        g_photo_buf = (uint8_t *)rt_malloc_align(bytes, 64);
        if (g_photo_buf != RT_NULL &&
            sd_asset_read_range(SD_GALLERY_PHOTOS_PATH, g_items[index].photo_offset,
                                g_photo_buf, bytes, RT_TRUE) == RT_EOK)
        {
            rt_memset(&g_photo_dsc, 0, sizeof(g_photo_dsc));
            g_photo_dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
            g_photo_dsc.header.cf     = LV_COLOR_FORMAT_RGB565;
            g_photo_dsc.header.flags  = LV_IMAGE_FLAGS_MODIFIABLE;
            g_photo_dsc.header.w      = g_items[index].w;
            g_photo_dsc.header.h      = g_items[index].h;
            g_photo_dsc.header.stride = g_items[index].w * 2U;
            g_photo_dsc.data_size     = bytes;
            g_photo_dsc.data          = g_photo_buf;

            lv_image_cache_drop(&g_photo_dsc);
            lv_obj_set_size(detail_image, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
            lv_image_set_src(detail_image, &g_photo_dsc);
            lv_image_set_pivot(detail_image, g_items[index].w / 2, g_items[index].h / 2);
            lv_image_set_scale(detail_image, 256);
            lv_obj_center(detail_image);
            load_ok = true;
        }
        else
        {
            gallery_free_photo();
        }
    }

    if (!load_ok && !sdcard_is_mounted())
    {
        lv_obj_add_flag(detail, LV_OBJ_FLAG_HIDDEN);
        gallery_sync_empty_state();
        return;
    }

    if (load_ok)
    {
        lv_label_set_text(caption, g_items[index].name);
    }
    else
    {
        lv_label_set_text(caption, g_items[index].is_gif
                                       ? "动图加载失败，请检查 TF 卡"
                                       : "照片加载失败，请检查 TF 卡");
    }
    lv_obj_add_flag(grid, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(empty_card, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(detail, LV_OBJ_FLAG_HIDDEN);
}

void ui_screen_gallery_on_leave(void)
{
    gallery_stop_gif();
    gallery_free_photo();
    if (detail != NULL) lv_obj_add_flag(detail, LV_OBJ_FLAG_HIDDEN);
    if (empty_card != NULL) lv_obj_add_flag(empty_card, LV_OBJ_FLAG_HIDDEN);
    gallery_free_thumbs();
    g_gallery_tried_sd_gen = 0U;
    if (grid != NULL) lv_obj_remove_flag(grid, LV_OBJ_FLAG_HIDDEN);
}

bool ui_screen_gallery_handle_swipe(lv_dir_t dir)
{
    if (detail == NULL || lv_obj_has_flag(detail, LV_OBJ_FLAG_HIDDEN))
        return false;

    if (dir == LV_DIR_LEFT)
    {
        ui_screen_gallery_open((selected + 1U) % ITEM_COUNT);
        return true;
    }
    if (dir == LV_DIR_RIGHT)
    {
        ui_screen_gallery_open((selected + ITEM_COUNT - 1U) % ITEM_COUNT);
        return true;
    }
    if (dir == LV_DIR_TOP)
    {
        gallery_stop_gif();
        gallery_free_photo();
        lv_obj_add_flag(detail, LV_OBJ_FLAG_HIDDEN);
        gallery_sync_empty_state();
        return true;
    }
    return false;
}

static void select_item(lv_event_t *event)
{
    ui_screen_gallery_open((unsigned)(uintptr_t)lv_event_get_user_data(event));
}

static void change_item(lv_event_t *event)
{
    unsigned step = (unsigned)(uintptr_t)lv_event_get_user_data(event);
    ui_screen_gallery_open((selected + step) % ITEM_COUNT);
}

static void show_grid(lv_event_t *event)
{
    (void)event;
    gallery_stop_gif();
    gallery_free_photo();
    lv_obj_add_flag(detail, LV_OBJ_FLAG_HIDDEN);
    gallery_sync_empty_state();
}

static void reload_btn_cb(lv_event_t *event)
{
    (void)event;
    g_gallery_tried_sd_gen = 0U;
    if (!sdcard_is_mounted() || !gallery_load_thumbs())
    {
        sdcard_request_remount();
        if (sdcard_is_present())
        {
            if (grid != NULL) lv_obj_add_flag(grid, LV_OBJ_FLAG_HIDDEN);
            if (empty_card != NULL) lv_obj_remove_flag(empty_card, LV_OBJ_FLAG_HIDDEN);
            if (empty_lbl != NULL)
            {
                ui_set_label_text(empty_lbl, "正在重新检测 TF 卡...\n请稍候");
            }
            return;
        }
    }
    gallery_sync_empty_state();
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, ui_font_zh_full(), 0);
    lv_obj_set_style_text_color(label, UI_COL_TEXT, 0);
    lv_label_set_text(label, text);
    return label;
}

static void make_detail_button(const char *text, int x, lv_event_cb_t cb, unsigned value)
{
    lv_obj_t *button = lv_button_create(detail);
    lv_obj_add_style(button, &g_st_btn_ghost, 0);
    lv_obj_set_size(button, 144, 48);
    lv_obj_align(button, LV_ALIGN_BOTTOM_LEFT, x, 0);
    lv_obj_center(make_label(button, text));
    lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, (void *)(uintptr_t)value);
}

lv_obj_t *ui_screen_gallery_create(void)
{
    lv_obj_t *screen = lv_obj_create(NULL);
    ui_create_chrome(screen, "图库 · TF卡", UI_SCR_GALLERY, &chrome);
    lv_obj_remove_flag(chrome.body, LV_OBJ_FLAG_SCROLLABLE);

    grid = lv_obj_create(chrome.body);
    lv_obj_remove_style_all(grid);
    lv_obj_remove_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(grid, lv_pct(100), lv_pct(100));
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(grid, 12, 0);

    for (unsigned i = 0; i < ITEM_COUNT; ++i)
    {
        lv_obj_t *tile = lv_button_create(grid);
        lv_obj_add_style(tile, &g_st_btn_ghost, 0);
        lv_obj_set_size(tile, 222, 300);
        lv_obj_set_style_pad_all(tile, 0, 0);
        if (g_items[i].is_gif)
        {
            lv_obj_set_style_border_color(tile, UI_COL_CYAN, 0);
        }
        lv_obj_add_event_cb(tile, select_item, LV_EVENT_CLICKED, (void *)(uintptr_t)i);

        thumb_imgs[i] = lv_image_create(tile);
        lv_obj_align(thumb_imgs[i], LV_ALIGN_TOP_MID, 0, 14);
        lv_obj_align(make_label(tile, g_items[i].name), LV_ALIGN_BOTTOM_MID, 0, -12);
    }

    empty_card = lv_obj_create(chrome.body);
    lv_obj_add_style(empty_card, &g_st_card, 0);
    lv_obj_set_size(empty_card, 420, 220);
    lv_obj_center(empty_card);
    lv_obj_remove_flag(empty_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(empty_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(empty_card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(empty_card, 18, 0);

    empty_lbl = make_label(empty_card, "未检测到 TF 卡");
    lv_obj_set_style_text_align(empty_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(empty_lbl, UI_COL_MUTED, 0);

    lv_obj_t *btn_reload = lv_button_create(empty_card);
    lv_obj_add_style(btn_reload, &g_st_btn_primary, 0);
    lv_obj_set_size(btn_reload, 168, 46);
    lv_obj_add_event_cb(btn_reload, reload_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *rlbl = lv_label_create(btn_reload);
    lv_label_set_text(rlbl, LV_SYMBOL_REFRESH " 重新加载");
    lv_obj_set_style_text_font(rlbl, ui_font_zh_full(), 0);
    lv_obj_set_style_text_color(rlbl, lv_color_hex(0x06101F), 0);
    lv_obj_center(rlbl);
    lv_obj_add_flag(empty_card, LV_OBJ_FLAG_HIDDEN);

    detail = lv_obj_create(chrome.body);
    lv_obj_remove_style_all(detail);
    lv_obj_remove_flag(detail, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(detail, lv_pct(100), lv_pct(100));
    image_box = lv_obj_create(detail);
    lv_obj_remove_style_all(image_box);
    lv_obj_remove_flag(image_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(image_box, 456, 576);
    lv_obj_align(image_box, LV_ALIGN_TOP_MID, 0, 0);
    detail_image = lv_image_create(image_box);
    caption = make_label(detail, "");
    lv_obj_align(caption, LV_ALIGN_BOTTOM_MID, 0, -54);
    make_detail_button(LV_SYMBOL_LEFT " 上一张", 0, change_item, ITEM_COUNT - 1);
    make_detail_button(LV_SYMBOL_IMAGE " 图库", 156, show_grid, 0);
    make_detail_button("下一张 " LV_SYMBOL_RIGHT, 312, change_item, 1);
    lv_obj_add_flag(detail, LV_OBJ_FLAG_HIDDEN);
    return screen;
}

void ui_screen_gallery_update(void)
{
    ui_update_chrome(&chrome);
    if (!sdcard_is_mounted())
    {
        if (g_gallery_sd_was_mounted || g_thumbs_ready ||
            (detail != NULL && !lv_obj_has_flag(detail, LV_OBJ_FLAG_HIDDEN)))
        {
            gallery_stop_gif();
            gallery_free_photo();
            gallery_free_thumbs();
            if (detail != NULL) lv_obj_add_flag(detail, LV_OBJ_FLAG_HIDDEN);
            g_gallery_tried_sd_gen = 0U;
            gallery_sync_empty_state();
        }
        g_gallery_sd_was_mounted = false;
        return;
    }

    g_gallery_sd_was_mounted = true;
    if (!g_thumbs_ready && g_gallery_tried_sd_gen != sdcard_get_mount_gen())
    {
        gallery_sync_empty_state();
    }
}
