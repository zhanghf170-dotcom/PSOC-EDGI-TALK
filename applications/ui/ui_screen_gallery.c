#include <rtthread.h>
#include "ui_manager.h"
#include "src/libs/gif/gifdec.h"

#define THUMB_W 192
#define THUMB_H 224
#define GIF_EDGE 320
#define RGB565_BYTES(w, h) ((w) * (h) * 2U)

extern const uint8_t gallery_photos[];
extern const uint8_t gallery_thumbnails[];
extern const uint8_t gallery_blink_gif[];

#define PHOTO(w_, h_, offset) { \
    .header = { .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565, \
                .w = (w_), .h = (h_), .stride = (w_) * 2 }, \
    .data_size = RGB565_BYTES(w_, h_), .data = (offset) \
}

static const lv_image_dsc_t photos[] = {
    PHOTO(256, 576, gallery_photos),
    PHOTO(448, 320, gallery_photos + RGB565_BYTES(256, 576)),
    PHOTO(448, 528, gallery_photos + RGB565_BYTES(256, 576) + RGB565_BYTES(448, 320)),
};

static const struct {
    const char *name;
    lv_image_dsc_t thumbnail;
} items[] = {
    {"照片 1", PHOTO(THUMB_W, THUMB_H, gallery_thumbnails)},
    {"照片 2", PHOTO(THUMB_W, THUMB_H, gallery_thumbnails + RGB565_BYTES(THUMB_W, THUMB_H))},
    {"照片 3", PHOTO(THUMB_W, THUMB_H, gallery_thumbnails + 2 * RGB565_BYTES(THUMB_W, THUMB_H))},
    {"动图", PHOTO(THUMB_W, THUMB_H, gallery_thumbnails + 3 * RGB565_BYTES(THUMB_W, THUMB_H))},
};

#define ITEM_COUNT (sizeof(items) / sizeof(items[0]))

static ui_chrome_t chrome;
static lv_obj_t *grid, *detail, *image_box, *detail_image, *caption;
static unsigned selected;
static gd_GIF *animation;
static lv_timer_t *animation_timer;
static lv_image_dsc_t animation_frame = PHOTO(GIF_EDGE, GIF_EDGE, NULL);

static void render_gif_frame(lv_timer_t *timer)
{
    if (gd_get_frame(animation) <= 0)
    {
        lv_timer_pause(timer);
        return;
    }
    gd_render_frame(animation, animation->canvas);

    const uint8_t *pixel = animation->canvas;
    uint16_t *output = (uint16_t *)animation_frame.data;
    for (unsigned i = 0; i < GIF_EDGE * GIF_EDGE; ++i, pixel += 4)
        output[i] = (uint16_t)(((pixel[2] & 0xF8U) << 8) | ((pixel[1] & 0xFCU) << 3) | (pixel[0] >> 3));

    lv_obj_invalidate(detail_image);
    lv_timer_set_period(timer, LV_MAX(1U, animation->gce.delay) * 10U);
}

static void close_animation(void)
{
    if (animation_timer != NULL)
    {
        lv_timer_delete(animation_timer);
        animation_timer = NULL;
    }
    if (animation != NULL)
    {
        gd_close_gif(animation);
        animation = NULL;
    }
    if (animation_frame.data != NULL)
    {
        lv_image_cache_drop(&animation_frame);
        rt_free_align((void *)animation_frame.data);
        animation_frame.data = NULL;
    }
}

static bool open_animation(void)
{
    animation = gd_open_gif_data(gallery_blink_gif);
    if (animation == NULL || animation->width != GIF_EDGE || animation->height != GIF_EDGE)
        goto fail;
    animation_frame.data = rt_malloc_align(animation_frame.data_size, LV_DRAW_BUF_ALIGN);
    if (animation_frame.data == NULL)
        goto fail;
    animation_timer = lv_timer_create(render_gif_frame, 80, NULL);
    if (animation_timer == NULL)
        goto fail;
    return true;

fail:
    close_animation();
    return false;
}

void ui_screen_gallery_open(unsigned index)
{
    if (index >= ITEM_COUNT)
        return;

    lv_image_set_src(detail_image, NULL);
    close_animation();
    selected = index;
    bool play = index >= sizeof(photos) / sizeof(photos[0]) && open_animation();
    const lv_image_dsc_t *source = &items[index].thumbnail;
    if (play)
        source = &animation_frame;
    else if (index < sizeof(photos) / sizeof(photos[0]))
        source = &photos[index];

    lv_image_set_src(detail_image, source);
    lv_image_set_scale(detail_image, play ? 352 : 256);
    lv_obj_center(detail_image);
    lv_label_set_text(caption, items[index].name);
    lv_obj_add_flag(grid, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(detail, LV_OBJ_FLAG_HIDDEN);
    if (play)
        render_gif_frame(animation_timer);
}

void ui_screen_gallery_on_leave(void)
{
    lv_obj_add_flag(detail, LV_OBJ_FLAG_HIDDEN);
    lv_image_set_src(detail_image, NULL);
    close_animation();
    lv_obj_remove_flag(grid, LV_OBJ_FLAG_HIDDEN);
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
    ui_screen_gallery_on_leave();
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, UI_FONT_ZH, 0);
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
    ui_create_chrome(screen, "图库", UI_SCR_GALLERY, &chrome);
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
        lv_obj_add_event_cb(tile, select_item, LV_EVENT_CLICKED, (void *)(uintptr_t)i);

        lv_obj_t *thumb = lv_image_create(tile);
        lv_image_set_src(thumb, &items[i].thumbnail);
        lv_obj_align(thumb, LV_ALIGN_TOP_MID, 0, 14);
        lv_obj_align(make_label(tile, items[i].name), LV_ALIGN_BOTTOM_MID, 0, -12);
    }

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
}
