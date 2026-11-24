#ifndef USBH_UVC_DISPLAY_HOOK_H
#define USBH_UVC_DISPLAY_HOOK_H

#include <stdint.h>
#include <rtthread.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t *framebuffer;
    uint16_t lcd_width;
    uint16_t lcd_height;
    uint16_t framebuffer_stride; /* Pixels between consecutive rows. */
    uint16_t src_width;
    uint16_t src_height;
    uint16_t dst_width;
    uint16_t dst_height;
    uint16_t dst_y_offset;
} uvc_display_overlay_info_t;

typedef void (*uvc_display_overlay_cb_t)(const uvc_display_overlay_info_t *info, void *user_ctx);

void uvc_display_set_overlay_callback(uvc_display_overlay_cb_t cb, void *user_ctx);

const uint16_t *uvc_display_get_stage_rgb565(uint16_t *w, uint16_t *h, uint32_t *seq);
void uvc_display_lock_stage(void);
void uvc_display_unlock_stage(void);

int uvc_app_start(uint8_t fmt, uint16_t w, uint16_t h);
int uvc_app_start_preview(void);
int uvc_app_start_monitor(void);
int uvc_app_stop(void);
rt_bool_t uvc_app_is_running(void);
rt_bool_t uvc_app_is_preview(void);
rt_bool_t uvc_app_preview_frame_ready(void);
void uvc_app_get_fps(uint32_t *cam_fps, uint32_t *disp_fps, uint32_t *drop_cnt);

#ifdef __cplusplus
}
#endif

#endif /* USBH_UVC_DISPLAY_HOOK_H */
