#ifndef UVC_AI_APP_H
#define UVC_AI_APP_H

#include <stdint.h>
#include <rtthread.h>
#include "usbh_uvc_stream.h"
#include "uvc_ai.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    rt_bool_t     running;
    rt_bool_t     ai_ready;
    rt_bool_t     alert_active;
    uint8_t       risk_level;       /* 0=Low/Safe, 1=Mid, 2=High */
    uint8_t       det_count;
    float         max_closed_conf;  /* 0.0f..1.0f */
    float         inference_ms;
    float         ai_fps;           /* Actual completed AI inferences/sec */
    uint32_t      persist_ms;
    uint32_t      persist_target_ms;
    uint32_t      danger_count;
    uvc_ai_mode_t mode;
    float         max_det_conf;     /* 0.0f..1.0f */
} uvc_ai_telemetry_t;

void uvc_ai_app_enforce_mode(uint8_t *fmt, uint16_t *width, uint16_t *height);
int  uvc_ai_app_start(uint16_t src_width, uint16_t src_height);
void uvc_ai_app_process_frame(const struct usbh_videoframe *frame, rt_tick_t frame_tick);
int  uvc_ai_app_stop(void);

int           uvc_ai_app_set_mode(uvc_ai_mode_t mode);
uvc_ai_mode_t uvc_ai_app_get_mode(void);

void     uvc_ai_app_get_telemetry(uvc_ai_telemetry_t *out);
rt_bool_t uvc_ai_app_is_failed(void);
void     uvc_ai_app_set_persist_ms(uint32_t ms);
uint32_t uvc_ai_app_get_persist_ms(void);

#ifdef __cplusplus
}
#endif

#endif /* UVC_AI_APP_H */
