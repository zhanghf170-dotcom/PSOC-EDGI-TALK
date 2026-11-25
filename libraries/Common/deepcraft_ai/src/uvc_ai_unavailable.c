#include <rtthread.h>
#include <string.h>
#include "uvc_ai.h"

#undef USB_DBG_TAG
#define USB_DBG_TAG "uvc_ai"
#include "usb_log.h"

static uvc_ai_mode_t g_mode = UVC_AI_MODE_FATIGUE;
static float g_ball_threshold = 0.65f;

void uvc_ai_set_mode(uvc_ai_mode_t mode)
{
    g_mode = mode == UVC_AI_MODE_STEEL_BALL ? UVC_AI_MODE_STEEL_BALL : UVC_AI_MODE_FATIGUE;
}

uvc_ai_mode_t uvc_ai_get_mode(void)
{
    return g_mode;
}

void uvc_ai_set_conf_threshold(float threshold)
{
    if (threshold < 0.15f)
        threshold = 0.15f;
    if (threshold > 0.95f)
        threshold = 0.95f;
    g_ball_threshold = threshold;
}

float uvc_ai_get_conf_threshold(void)
{
    return g_ball_threshold;
}

int uvc_ai_init(const uvc_ai_config_t *config)
{
    (void)config;
    USB_LOG_WRN("Install the local vision models to enable AI detection\r\n");
    return -RT_ENOSYS;
}

void uvc_ai_deinit(void)
{
}

void uvc_ai_reset_result(uvc_ai_result_t *result)
{
    if (result != RT_NULL)
        memset(result, 0, sizeof(*result));
}

int uvc_ai_process_yuyv(const uint8_t *yuyv, uint32_t yuyv_size, uvc_ai_result_t *result)
{
    (void)yuyv;
    (void)yuyv_size;
    uvc_ai_reset_result(result);
    return -RT_ENOSYS;
}
