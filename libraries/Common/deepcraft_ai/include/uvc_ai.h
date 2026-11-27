#ifndef UVC_AI_H
#define UVC_AI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UVC_AI_CAMERA_WIDTH      320U
#define UVC_AI_CAMERA_HEIGHT     240U
#define UVC_AI_IMAGE_WIDTH       320U
#define UVC_AI_IMAGE_HEIGHT      320U
#define UVC_AI_NUM_CLASSES       3U
#define UVC_AI_MAX_CLASS_LEN     12U
#define UVC_AI_MAX_PREDICTIONS   32U
#define UVC_AI_FRAMES_TO_SKIP    1U

typedef enum {
    UVC_AI_MODE_FATIGUE = 0,
    UVC_AI_MODE_STEEL_BALL = 1,
} uvc_ai_mode_t;

typedef struct {
    uint8_t initialized;
    uint16_t src_width;
    uint16_t src_height;
} uvc_ai_config_t;

typedef struct {
    uint8_t valid;
    uint8_t count;
    float inference_ms;
    int16_t bbox_int16[UVC_AI_MAX_PREDICTIONS * 4U];
    float conf[UVC_AI_MAX_PREDICTIONS];
    uint8_t class_id[UVC_AI_MAX_PREDICTIONS];
    uint16_t track_id[UVC_AI_MAX_PREDICTIONS];
    int16_t single_track_vx;   /* Pixels per second in camera coordinates. */
    int16_t single_track_vy;
    union {
        char label[UVC_AI_MAX_PREDICTIONS][UVC_AI_MAX_CLASS_LEN];
        char class_string[UVC_AI_MAX_PREDICTIONS][UVC_AI_MAX_CLASS_LEN];
    };
} uvc_ai_result_t;

void uvc_ai_set_mode(uvc_ai_mode_t mode);
uvc_ai_mode_t uvc_ai_get_mode(void);
uvc_ai_mode_t uvc_ai_get_init_mode(void);
void uvc_ai_set_conf_threshold(float thresh);
float uvc_ai_get_conf_threshold(void);
uint32_t uvc_ai_get_npu_timeout_count(void);

int uvc_ai_init(const uvc_ai_config_t *config);
void uvc_ai_deinit(void);
void uvc_ai_reset_result(uvc_ai_result_t *result);
int uvc_ai_process_yuyv(const uint8_t *yuyv, uint32_t yuyv_size, uvc_ai_result_t *result);
int uvc_ai_process_yuyv_ex(const uint8_t *yuyv, uint32_t yuyv_size,
                           uvc_ai_result_t *high_result,
                           uvc_ai_result_t *low_result);
void uvc_ai_invalidate_steel_ball_cache(void);

#ifdef __cplusplus
}
#endif

#endif /* UVC_AI_H */
