#include <rtthread.h>
#include "ui_manager.h"
#include "usbh_uvc_display_hook.h"

#define CAMERA_W 320
#define CAMERA_H 240
#define CAMERA_TIMEOUT_MS 1800U

static ui_chrome_t chrome;
static lv_obj_t *view, *message, *retry_button;
static uint32_t last_sequence, last_frame_at;
static lv_image_dsc_t preview = {
    .header = { .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565,
                .w = CAMERA_W, .h = CAMERA_H, .stride = CAMERA_W * 2 },
    .data_size = CAMERA_W * CAMERA_H * 2U,
};

static void show_message(const char *text, bool can_retry)
{
    lv_obj_add_flag(view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(message, LV_OBJ_FLAG_HIDDEN);
    ui_set_label_text(message, text);
    if (can_retry)
        lv_obj_remove_flag(retry_button, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(retry_button, LV_OBJ_FLAG_HIDDEN);
}

static void start_preview(void)
{
    if (preview.data == NULL)
    {
        preview.data = rt_malloc_align(preview.data_size, LV_DRAW_BUF_ALIGN);
        if (preview.data == NULL)
        {
            (void)uvc_app_stop();
            show_message("无法打开预览", true);
            return;
        }
        lv_image_set_src(view, &preview);
    }

    (void)uvc_display_get_stage_rgb565(NULL, NULL, &last_sequence);
    last_frame_at = lv_tick_get();
    if (uvc_app_start_preview() == RT_EOK)
        show_message("等待相机画面", false);
    else
        show_message("无法打开相机", true);
}

static void retry_preview(lv_event_t *event)
{
    (void)event;
    if (uvc_app_is_preview())
        (void)uvc_app_stop();
    start_preview();
}

lv_obj_t *ui_screen_camera_create(void)
{
    lv_obj_t *screen = lv_obj_create(NULL);
    ui_create_chrome(screen, "相机", UI_SCR_CAMERA, &chrome);
    lv_obj_remove_flag(chrome.body, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *frame = lv_obj_create(chrome.body);
    lv_obj_add_style(frame, &g_st_card, 0);
    lv_obj_set_style_bg_color(frame, lv_color_hex(0x050811), 0);
    lv_obj_set_style_pad_all(frame, 0, 0);
    lv_obj_remove_flag(frame, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(frame, 456, 346);
    lv_obj_align(frame, LV_ALIGN_TOP_MID, 0, 116);

    view = lv_image_create(frame);
    lv_image_set_scale(view, 352);
    lv_obj_center(view);
    lv_obj_add_flag(view, LV_OBJ_FLAG_HIDDEN);

    message = lv_label_create(frame);
    lv_obj_set_style_text_color(message, UI_COL_MUTED, 0);
    lv_obj_set_style_text_font(message, UI_FONT_ZH, 0);
    lv_obj_center(message);

    retry_button = lv_button_create(chrome.body);
    lv_obj_add_style(retry_button, &g_st_btn_primary, 0);
    lv_obj_set_size(retry_button, 200, 48);
    lv_obj_align_to(retry_button, frame, LV_ALIGN_OUT_BOTTOM_MID, 0, 22);
    lv_obj_add_event_cb(retry_button, retry_preview, LV_EVENT_CLICKED, NULL);
    lv_obj_t *label = lv_label_create(retry_button);
    lv_obj_set_style_text_font(label, UI_FONT_ZH, 0);
    lv_label_set_text(label, LV_SYMBOL_REFRESH " 重试");
    lv_obj_center(label);
    return screen;
}

void ui_screen_camera_on_enter(void)
{
    start_preview();
}

void ui_screen_camera_on_leave(bool enter_monitor)
{
    if (!enter_monitor && uvc_app_is_preview())
        (void)uvc_app_stop();
    lv_obj_add_flag(view, LV_OBJ_FLAG_HIDDEN);
    lv_image_set_src(view, NULL);
    if (preview.data != NULL)
    {
        lv_image_cache_drop(&preview);
        rt_free_align((void *)preview.data);
        preview.data = NULL;
    }
}

void ui_screen_camera_update(void)
{
    ui_update_chrome(&chrome);
    if (preview.data == NULL)
        return;
    if (!uvc_app_is_preview())
    {
        show_message("连接 USB 摄像头后重试", true);
        return;
    }

    uint32_t sequence;
    uvc_display_lock_stage();
    const uint16_t *stage = uvc_display_get_stage_rgb565(NULL, NULL, &sequence);
    bool new_frame = uvc_app_preview_frame_ready() && sequence != last_sequence;
    if (new_frame)
        rt_memcpy((void *)preview.data, stage, preview.data_size);
    uvc_display_unlock_stage();

    if (new_frame)
    {
        last_sequence = sequence;
        last_frame_at = lv_tick_get();
        lv_obj_add_flag(message, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(retry_button, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(view, LV_OBJ_FLAG_HIDDEN);
        lv_obj_invalidate(view);
    }
    else if (lv_tick_elaps(last_frame_at) > CAMERA_TIMEOUT_MS)
    {
        show_message("连接 USB 摄像头后重试", true);
    }
}
