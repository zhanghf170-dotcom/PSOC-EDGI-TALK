/*
 * CherryUSB UVC Host Application Layer
 * FPS monitoring, msh commands, video device callbacks, frame processing.
 */

#include <rtthread.h>
#include <rtdevice.h>
#include <stdlib.h>
#include "usbh_uvc_stream.h"
#include "usbh_uvc_display_hook.h"
#include "wifi_stream.h"

#ifdef BSP_USING_DEEPCRAFT_AI
#include "uvc_ai_app.h"
#else
static void uvc_ai_app_enforce_mode(uint8_t *fmt, uint16_t *width, uint16_t *height)
{
    (void)fmt;
    (void)width;
    (void)height;
}

static int uvc_ai_app_start(uint16_t src_width, uint16_t src_height)
{
    (void)src_width;
    (void)src_height;
    return 0;
}

static void uvc_ai_app_process_frame(const struct usbh_videoframe *frame)
{
    (void)frame;
}

static void uvc_ai_app_stop(void)
{
}
#endif

#undef  USB_DBG_TAG
#define USB_DBG_TAG "uvc_app"
#include "usb_log.h"

int uvc_display_init(void);
void uvc_display_frame(struct usbh_videoframe *frame, uint16_t src_w, uint16_t src_h);

/* ---------- frame buffers ---------- */

/*
 * Use one raw-sized backing store for both YUYV and MJPEG.
 * This keeps MJPEG safe even when the camera uses a larger-than-expected
 * compressed frame, while preserving the existing YUYV path.
 */
#define UVC_FRAME_BUF_SIZE  (640 * 480 * 2)
#define UVC_FRAME_BUF_COUNT 4

static USB_MEM_ALIGNX uint8_t frame_buffer[UVC_FRAME_BUF_COUNT][UVC_FRAME_BUF_SIZE]
    __attribute__((section(".m33_m55_shared_hyperram")));
static struct usbh_videoframe frame_pool[UVC_FRAME_BUF_COUNT];

static volatile uint8_t uvc_app_running;
static volatile rt_bool_t uvc_preview_only;
static volatile rt_bool_t uvc_preview_frame_ready;
static volatile uint8_t g_uvc_frm_exited = 1;
static volatile uint8_t g_uvc_fps_exited = 1;
static volatile uint32_t g_uvc_display_fps;
static volatile uint32_t g_uvc_display_drop_count;
static struct rt_mutex uvc_control_lock;

static int uvc_control_init(void)
{
    return rt_mutex_init(&uvc_control_lock, "uvc_ctrl", RT_IPC_FLAG_PRIO);
}
INIT_PREV_EXPORT(uvc_control_init);

/* The BSP initializes its URBs asynchronously in usbh_video. Do not ask
 * the stop routine to cancel them until that thread has finished opening. */
static void uvc_stop_transport(void)
{
    if (!uvc_app_running)
        return;

    uvc_app_running = 0;
    while (rt_thread_find("usbh_video") != RT_NULL)
        rt_thread_mdelay(5);
    usbh_video_stream_stop();
}

static void uvc_filter_empty_formats(struct usbh_video *video_class)
{
    uint8_t read_idx;
    uint8_t write_idx;

    if (video_class == RT_NULL) {
        return;
    }

    /*
     * Some cameras expose an extra VS format entry with zero frames.
     * Keep CherryUSB core unchanged and compact valid entries here,
     * so usbh_video_open() does not pick an empty format.
     */
    write_idx = 0U;
    for (read_idx = 0U; read_idx < video_class->num_of_formats; read_idx++) {
        if (video_class->format[read_idx].num_of_frames == 0U) {
            continue;
        }

        if (write_idx != read_idx) {
            video_class->format[write_idx] = video_class->format[read_idx];
        }
        write_idx++;
    }

    if (write_idx != video_class->num_of_formats) {
        USB_LOG_WRN("UVC filter empty format: %u -> %u\r\n",
                    video_class->num_of_formats, write_idx);
        video_class->num_of_formats = write_idx;
    }
}

/* ---------- CherryUSB video class callbacks ---------- */

void usbh_video_run(struct usbh_video *video_class)
{
    uvc_filter_empty_formats(video_class);
    USB_LOG_INFO("UVC device connected\r\n");
    usbh_video_list_info(video_class);
}

void usbh_video_stop(struct usbh_video *video_class)
{
    USB_LOG_INFO("UVC device disconnected\r\n");
    rt_mutex_take(&uvc_control_lock, RT_WAITING_FOREVER);
    uvc_stop_transport();
    rt_mutex_release(&uvc_control_lock);
}

/* ---------- FPS print thread ---------- */

extern volatile uint32_t g_uvc_fps;
extern volatile uint32_t uvc_transfer_count;
extern volatile uint32_t video_complete_count;

static void uvc_display_fps_record(void)
{
    static uint32_t frame_count;
    static rt_tick_t tick_last;
    rt_tick_t tick_now;
    rt_tick_t tick_delta;

    frame_count++;
    if ((frame_count % 10U) != 0U) {
        return;
    }

    tick_now = rt_tick_get();
    if (tick_last == 0U) {
        tick_last = tick_now;
        return;
    }

    tick_delta = tick_now - tick_last;
    if (tick_delta > 0U) {
        g_uvc_display_fps = (10U * RT_TICK_PER_SECOND) / tick_delta;
    }
    tick_last = tick_now;
}

static void uvc_frame_keep_latest(struct usbh_videoframe **frame)
{
    struct usbh_videoframe *latest = *frame;
    struct usbh_videoframe *queued;

    while (usbh_video_stream_dequeue(&queued, 0) == 0) {
        usbh_video_stream_enqueue(latest);
        latest = queued;
        g_uvc_display_drop_count++;
    }

    *frame = latest;
}

static void uvc_fps_thread_entry(void *arg)
{
    uint32_t delay_ms;
    (void)arg;

    while (uvc_app_running) {
        USB_LOG_INFO("UVC fps:%d disp:%d drop:%u urb:%d\r\n",
                     (int)g_uvc_fps, (int)g_uvc_display_fps,
                     (unsigned)g_uvc_display_drop_count,
                     (int)video_complete_count);
        for (delay_ms = 0; (delay_ms < 3000U) && uvc_app_running; delay_ms += 50U)
        {
            rt_thread_mdelay(50);
        }
    }
    g_uvc_fps_exited = 1;
}

/* ---------- frame consumer thread ---------- */

static uint16_t uvc_cam_w, uvc_cam_h;

static void uvc_frame_thread_entry(void *arg)
{
    struct usbh_videoframe *frame;
    uint32_t frame_count = 0;
    int ret;
    uint16_t stream_w;
    uint16_t stream_h;
    rt_bool_t monitoring = RT_FALSE;
    (void)arg;

    /* initialise display output */
    uvc_display_init();

    while (uvc_app_running) {
        rt_bool_t preview_only = uvc_preview_only;
        if (preview_only && monitoring) {
            uvc_ai_app_stop();
            monitoring = RT_FALSE;
        } else if (!preview_only && !monitoring) {
            (void)uvc_ai_app_start(uvc_cam_w, uvc_cam_h);
            monitoring = RT_TRUE;
        }

        ret = usbh_video_stream_dequeue(&frame, 200);
        if (ret < 0)
            continue;

        uvc_frame_keep_latest(&frame);

        frame_count++;
        if ((frame_count % 30) == 1) {
            USB_LOG_INFO("frame #%d  size=%u fmt=%s\r\n",
                         (int)frame_count, (unsigned)frame->frame_size,
                         frame->frame_format == USBH_VIDEO_FORMAT_MJPEG ?
                             "mjpeg" : "yuyv");
        }

        usbh_video_stream_get_info(&stream_w, &stream_h, RT_NULL);
        {
            uint16_t eff_w = stream_w ? stream_w : uvc_cam_w;
            uint16_t eff_h = stream_h ? stream_h : uvc_cam_h;

            if (monitoring && !uvc_preview_only) {
                uvc_ai_app_process_frame(frame);
                wifi_stream_submit_frame(frame->frame_buf, frame->frame_size,
                                         eff_w, eff_h, frame->frame_format);
            }

            /* render frame to LVGL stage or LCD */
            uvc_display_frame(frame, eff_w, eff_h);
            if (preview_only && uvc_preview_only)
                uvc_preview_frame_ready = RT_TRUE;
        }
        uvc_display_fps_record();

        usbh_video_stream_enqueue(frame);
    }

    if (monitoring)
        uvc_ai_app_stop();
    g_uvc_frm_exited = 1;
}

/* ---------- public API & msh commands ---------- */

static int uvc_app_start_mode(uint8_t fmt, uint16_t w, uint16_t h, rt_bool_t preview_only)
{
    uint32_t frame_bufsize = UVC_FRAME_BUF_SIZE;
    int ret;
    rt_thread_t t;

    if (uvc_app_running) {
        USB_LOG_WRN("UVC already running, stop first\r\n");
        return -RT_EBUSY;
    }

    /* Wait briefly if previous stop is still winding down */
    for (int i = 0; i < 20 && (!g_uvc_frm_exited || !g_uvc_fps_exited); i++) {
        rt_thread_mdelay(25);
    }

    if (uvc_app_running || !g_uvc_frm_exited || !g_uvc_fps_exited)
        return -RT_EBUSY;

    if (!preview_only)
        uvc_ai_app_enforce_mode(&fmt, &w, &h);

    USB_LOG_INFO("UVC start: %ux%u format=%s\r\n", w, h,
                 fmt == USBH_VIDEO_FORMAT_MJPEG ? "mjpeg" : "yuyv");

    if (fmt == USBH_VIDEO_FORMAT_UNCOMPRESSED) {
        uint32_t yuyv_size = (uint32_t)w * (uint32_t)h * 2U;

        if ((w == 0U) || (h == 0U) || (yuyv_size > UVC_FRAME_BUF_SIZE)) {
            USB_LOG_ERR("Invalid YUYV frame size: %ux%u (%lu)\r\n",
                        w, h, (unsigned long)yuyv_size);
            return -RT_EINVAL;
        }
        frame_bufsize = yuyv_size;
    }

    /* initialise frame pool */
    for (uint32_t i = 0; i < UVC_FRAME_BUF_COUNT; i++) {
        frame_pool[i].frame_buf = frame_buffer[i];
        frame_pool[i].frame_bufsize = frame_bufsize;
    }

    uvc_cam_w = w;
    uvc_cam_h = h;
    g_uvc_display_fps = 0;
    g_uvc_display_drop_count = 0;

    ret = usbh_video_stream_create(frame_pool, UVC_FRAME_BUF_COUNT);
    if (ret < 0) {
        USB_LOG_ERR("UVC frame pool create failed: %d\r\n", ret);
        return ret;
    }

    g_uvc_fps_exited = 0;
    g_uvc_frm_exited = 0;
    uvc_preview_frame_ready = RT_FALSE;
    uvc_preview_only = preview_only;
    uvc_app_running = 1;

    t = rt_thread_create("uvc_fps", uvc_fps_thread_entry, NULL,
                         1024, 20, 10);
    if (t) {
        rt_thread_startup(t);
    } else {
        g_uvc_fps_exited = 1;
    }

    t = rt_thread_create("uvc_frm", uvc_frame_thread_entry, NULL,
                         32768, 22, 10);
    if (t) {
        rt_thread_startup(t);
    } else {
        g_uvc_frm_exited = 1;
        uvc_app_running = 0;
        return -RT_ENOMEM;
    }

    /* start the video stream (frames are queued inside stream_start) */
    usbh_video_stream_start(w, h, fmt);
    return RT_EOK;
}

static int uvc_app_request_mode(uint8_t fmt, uint16_t w, uint16_t h,
                                rt_bool_t preview_only, rt_bool_t reuse_stream)
{
    int ret;

    rt_mutex_take(&uvc_control_lock, RT_WAITING_FOREVER);
    if (reuse_stream && uvc_app_running) {
        if (uvc_preview_only != preview_only) {
            uvc_preview_frame_ready = RT_FALSE;
            uvc_preview_only = preview_only;
        }
        ret = RT_EOK;
    } else {
        ret = uvc_app_start_mode(fmt, w, h, preview_only);
    }
    rt_mutex_release(&uvc_control_lock);
    return ret;
}

int uvc_app_start(uint8_t fmt, uint16_t w, uint16_t h)
{
    return uvc_app_request_mode(fmt, w, h, RT_FALSE, RT_FALSE);
}

int uvc_app_start_preview(void)
{
    return uvc_app_request_mode(USBH_VIDEO_FORMAT_UNCOMPRESSED, 320, 240, RT_TRUE, RT_TRUE);
}

int uvc_app_start_monitor(void)
{
    return uvc_app_request_mode(USBH_VIDEO_FORMAT_UNCOMPRESSED, 320, 240, RT_FALSE, RT_TRUE);
}

int uvc_app_stop(void)
{
    int wait_ms;

    rt_mutex_take(&uvc_control_lock, RT_WAITING_FOREVER);
    if (!uvc_app_running && g_uvc_frm_exited && g_uvc_fps_exited) {
        rt_mutex_release(&uvc_control_lock);
        return RT_EOK;
    }

    uvc_stop_transport();

    for (wait_ms = 0; wait_ms < 500; wait_ms += 20) {
        if (g_uvc_frm_exited && g_uvc_fps_exited) {
            break;
        }
        rt_thread_mdelay(20);
    }

    g_uvc_display_fps = 0;
    USB_LOG_INFO("UVC stopped\r\n");
    rt_mutex_release(&uvc_control_lock);
    return RT_EOK;
}

rt_bool_t uvc_app_is_running(void)
{
    return uvc_app_running ? RT_TRUE : RT_FALSE;
}

rt_bool_t uvc_app_is_preview(void)
{
    return uvc_app_running && uvc_preview_only;
}

rt_bool_t uvc_app_preview_frame_ready(void)
{
    return uvc_app_is_preview() && uvc_preview_frame_ready;
}

void uvc_app_get_fps(uint32_t *cam_fps, uint32_t *disp_fps, uint32_t *drop_cnt)
{
    if (cam_fps) *cam_fps = g_uvc_fps;
    if (disp_fps) *disp_fps = g_uvc_display_fps;
    if (drop_cnt) *drop_cnt = g_uvc_display_drop_count;
}

static int cmd_usbh_uvc_start(int argc, char **argv)
{
    uint8_t fmt = USBH_VIDEO_FORMAT_UNCOMPRESSED;
    uint16_t w = 320;
    uint16_t h = 240;

    if (argc >= 2) {
        fmt = (uint8_t)atoi(argv[1]);
    }
    if (argc >= 4) {
        w = (uint16_t)atoi(argv[2]);
        h = (uint16_t)atoi(argv[3]);
    }

    return uvc_app_start(fmt, w, h);
}
MSH_CMD_EXPORT_ALIAS(cmd_usbh_uvc_start, usbh_uvc_start,
                     Start UVC stream: usbh_uvc_start [fmt] [w] [h]);

static int cmd_usbh_uvc_stop(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return uvc_app_stop();
}
MSH_CMD_EXPORT_ALIAS(cmd_usbh_uvc_stop, usbh_uvc_stop, Stop UVC stream);
