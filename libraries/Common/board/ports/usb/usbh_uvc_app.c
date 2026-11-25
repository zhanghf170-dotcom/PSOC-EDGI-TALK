/*
 * CherryUSB UVC Host Application Layer
 * FPS monitoring, msh commands, video device callbacks, frame processing.
 */

#include <rtthread.h>
#include <rtdevice.h>
#include <stdlib.h>
#include "usbh_uvc_stream.h"
#include "usbh_hub.h"
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

static int uvc_ai_app_stop(void)
{
    return RT_EOK;
}
#endif

#undef  USB_DBG_TAG
#define USB_DBG_TAG "uvc_app"
#include "usb_log.h"

int uvc_display_init(void);
void uvc_display_frame(struct usbh_videoframe *frame, uint16_t src_w, uint16_t src_h);

#ifdef BSP_USING_DEEPCRAFT_AI
#define UVC_FRAME_BYTES (320U * 240U * 2U)
#else
#define UVC_FRAME_BYTES (640U * 480U * 2U)
#endif
#define UVC_FRAME_COUNT 4U
#define UVC_FIRST_FRAME_MS 4500
#define UVC_NO_FRAME_MS 2200
#define UVC_STOP_MS 2000
#define UVC_ISO_RETRY_LIMIT 3U
#define UVC_REOPEN_QUIET_MS 450
#define UVC_REOPEN_MAX_SHIFT 2U
#define UVC_FAILURES_BEFORE_REPROBE 3U

enum uvc_mode
{
    UVC_MODE_OFF,
    UVC_MODE_PREVIEW,
    UVC_MODE_MONITOR,
};

enum uvc_state
{
    UVC_IDLE,
    UVC_STARTING,
    UVC_STREAMING,
    UVC_STOPPING,
    UVC_FAILED,
};

struct uvc_request
{
    uint32_t id;
    enum uvc_mode mode;
    uint16_t width;
    uint16_t height;
    uint8_t format;
};

static USB_MEM_ALIGNX uint8_t frame_buffer[UVC_FRAME_COUNT][UVC_FRAME_BYTES]
    __attribute__((section(".m33_m55_shared_hyperram")));
static struct usbh_videoframe frame_pool[UVC_FRAME_COUNT];
static rt_bool_t pool_created;

static struct uvc_request requested;
static struct rt_semaphore control_wake, frame_wake;
static struct rt_thread control_thread, frame_thread;
rt_align(RT_ALIGN_SIZE) static uint8_t control_stack[3072];
rt_align(RT_ALIGN_SIZE) static uint8_t frame_stack[32768];
static rt_bool_t control_ready;

static volatile enum uvc_state camera_state;
static volatile enum uvc_mode capture_mode;
static volatile rt_bool_t capture_run;
static volatile rt_bool_t capture_idle = RT_TRUE;
static volatile rt_bool_t camera_attached;
static struct usbh_hubport *volatile camera_port;
static volatile int open_result;
static volatile int camera_error;
static volatile rt_tick_t frame_tick;
static volatile uint32_t frames_received;
static volatile uint32_t frames_dropped;
static volatile uint32_t display_fps;
static volatile uint32_t transport_starts;
static volatile uint32_t suppressed_urb_callbacks;
static volatile uint32_t recovered_urb_errors;

static uint16_t capture_width;
static uint16_t capture_height;
extern volatile uint32_t g_uvc_fps;

#ifdef UVC_GUARD_USB_CALLBACKS
static const struct usbh_hubport *uvc_stream_port;
static const struct usb_endpoint_descriptor *volatile uvc_stream_endpoint;
static struct uvc_urb_callback
{
    struct usbh_urb *urb;
    usbh_complete_callback_t complete;
    void *arg;
    unsigned int retries;
} uvc_callbacks[2];

int __real_usbh_submit_urb(struct usbh_urb *urb);

static void uvc_complete_urb(void *arg, int nbytes)
{
    struct uvc_urb_callback *callback = arg;

    /* During stop the library cancels and frees the two URBs in sequence.
     * Suppress both callbacks before one of them could requeue its peer. */
    if (capture_run && camera_attached && uvc_stream_endpoint)
    {
        /* An isochronous transaction can fail while the device is still attached.
         * Requeue the URB without publishing its partial video data. */
        if (nbytes == -USB_ERR_IO &&
            (callback->urb->ep->bmAttributes & USB_ENDPOINT_TYPE_MASK) == USB_ENDPOINT_TYPE_ISOCHRONOUS &&
            callback->retries < UVC_ISO_RETRY_LIMIT)
        {
            callback->retries++;
            if (__real_usbh_submit_urb(callback->urb) == 0)
            {
                recovered_urb_errors++;
                return;
            }
        }
        if (nbytes == 0)
            callback->retries = 0;
        callback->complete(callback->arg, nbytes);
        return;
    }
    suppressed_urb_callbacks++;
}

int __wrap_usbh_submit_urb(struct usbh_urb *urb)
{
    rt_base_t level = rt_hw_interrupt_disable();
    if (!urb || !uvc_stream_endpoint || urb->hport != uvc_stream_port ||
        urb->ep != uvc_stream_endpoint || !urb->complete)
    {
        rt_hw_interrupt_enable(level);
        return __real_usbh_submit_urb(urb);
    }
    if (!capture_run || !camera_attached)
    {
        rt_hw_interrupt_enable(level);
        return -USB_ERR_SHUTDOWN;
    }
    if (urb->complete != uvc_complete_urb)
    {
        struct uvc_urb_callback *slot = RT_NULL;
        for (unsigned int i = 0; i < ARRAY_SIZE(uvc_callbacks); ++i)
        {
            if (!uvc_callbacks[i].urb || uvc_callbacks[i].urb == urb)
            {
                slot = &uvc_callbacks[i];
                break;
            }
        }
        if (!slot)
        {
            rt_hw_interrupt_enable(level);
            return -USB_ERR_NOMEM;
        }
        slot->urb = urb;
        slot->complete = urb->complete;
        slot->arg = urb->arg;
        slot->retries = 0;
        urb->arg = slot;
        urb->complete = uvc_complete_urb;
    }
    rt_hw_interrupt_enable(level);
    return __real_usbh_submit_urb(urb);
}
#endif

static struct uvc_request uvc_read_request(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    struct uvc_request req = requested;
    rt_hw_interrupt_enable(level);
    return req;
}

static void uvc_fail_request(uint32_t request_id, int error)
{
    rt_base_t level = rt_hw_interrupt_disable();
    if (requested.id == request_id && requested.mode != UVC_MODE_OFF)
    {
        requested.mode = UVC_MODE_OFF;
        camera_error = error;
    }
    rt_hw_interrupt_enable(level);
}

void usbh_video_run(struct usbh_video *video_class)
{
    camera_port = video_class->hport;
    camera_attached = RT_TRUE;
    USB_LOG_INFO("UVC device connected\r\n");
    if (control_ready)
        rt_sem_release(&control_wake);
}

void usbh_video_stop(struct usbh_video *video_class)
{
    (void)video_class;
    rt_base_t level = rt_hw_interrupt_disable();
    camera_attached = RT_FALSE;
    camera_port = RT_NULL;
#ifdef UVC_GUARD_USB_CALLBACKS
    uvc_stream_endpoint = RT_NULL;
#endif
    if (requested.mode != UVC_MODE_OFF)
    {
        camera_error = -USB_ERR_NODEV;
        open_result = -USB_ERR_NODEV;
    }
    rt_hw_interrupt_enable(level);
    USB_LOG_INFO("UVC device disconnected\r\n");
    if (control_ready)
        rt_sem_release(&control_wake);
}

void usbh_video_stream_open_result(const struct usbh_video *video_class, int result)
{
#ifdef UVC_GUARD_USB_CALLBACKS
    rt_base_t level = rt_hw_interrupt_disable();
    if (result >= 0 && camera_attached && video_class)
    {
        uvc_stream_port = video_class->hport;
        uvc_stream_endpoint = video_class->is_bulk ? video_class->bulkin : video_class->isoin;
    }
    rt_hw_interrupt_enable(level);
#else
    (void)video_class;
#endif
    open_result = result;
    if (control_ready)
        rt_sem_release(&control_wake);
}

/* Keep transport restarts in the same task that owns start and stop. */
void __wrap_usbh_video_transfer_abort_callback(void)
{
    if (capture_run)
    {
        open_result = -USB_ERR_IO;
        rt_sem_release(&control_wake);
    }
}

static rt_bool_t uvc_elapsed(rt_tick_t now, rt_tick_t start, int ms)
{
    return (rt_tick_t)(now - start) >= rt_tick_from_millisecond(ms);
}

static rt_bool_t uvc_retryable_error(int error)
{
    return error == -USB_ERR_IO || error == -USB_ERR_TIMEOUT ||
           error == -RT_ETIMEOUT || error == -USB_ERR_BUSY || error == -USB_ERR_NAK;
}

static void uvc_frame_keep_latest(struct usbh_videoframe **frame)
{
    struct usbh_videoframe *queued;
    for (unsigned int i = 1; i < UVC_FRAME_COUNT; ++i)
    {
        if (usbh_video_stream_dequeue(&queued, 0) != 0)
            break;
        usbh_video_stream_enqueue(*frame);
        *frame = queued;
        frames_dropped++;
    }
}

static void uvc_frame_thread_entry(void *arg)
{
    (void)arg;
    for (;;)
    {
        rt_sem_take(&frame_wake, RT_WAITING_FOREVER);
        rt_bool_t monitoring = RT_FALSE;
        if (capture_run && uvc_display_init() != RT_EOK)
        {
            open_result = -RT_EIO;
            rt_sem_release(&control_wake);
        }

        while (capture_run)
        {
            if (monitoring && capture_mode != UVC_MODE_MONITOR)
            {
                if (uvc_ai_app_stop() != RT_EOK)
                    continue;
                monitoring = RT_FALSE;
            }

            struct usbh_videoframe *frame;
            if (usbh_video_stream_dequeue(&frame, 60) < 0)
                continue;
            uvc_frame_keep_latest(&frame);

            uint16_t w = 0, h = 0;
            usbh_video_stream_get_info(&w, &h, RT_NULL);
            if (w == 0U) w = capture_width;
            if (h == 0U) h = capture_height;
            if (capture_run && frame->frame_buf != RT_NULL && frame->frame_size > 0U &&
                frame->frame_size <= frame->frame_bufsize &&
                (frame->frame_format != USBH_VIDEO_FORMAT_UNCOMPRESSED ||
                 frame->frame_size >= (uint32_t)w * h * 2U))
            {
                uvc_display_frame(frame, w, h);
                frame_tick = rt_tick_get();
                frames_received++;
                if (frames_received == 1U)
                {
                    USB_LOG_INFO("UVC first frame: %ux%u %lu B\r\n", w, h, (unsigned long)frame->frame_size);
                    rt_sem_release(&control_wake);
                }
                if (capture_run && capture_mode == UVC_MODE_MONITOR)
                {
                    if (!monitoring)
                        monitoring = uvc_ai_app_start(w, h) == RT_EOK;
                    if (monitoring)
                        uvc_ai_app_process_frame(frame);
                }
                if (capture_run && capture_mode != UVC_MODE_OFF)
                {
                    wifi_stream_submit_frame(frame->frame_buf, frame->frame_size, w, h, frame->frame_format);
                }
            }
            usbh_video_stream_enqueue(frame);
        }

        if (monitoring)
        {
            while (uvc_ai_app_stop() != RT_EOK)
                rt_thread_mdelay(25);
        }
        uvc_display_release_scratch();
        wifi_stream_camera_stopped();
        capture_idle = RT_TRUE;
        rt_sem_release(&control_wake);
    }
}

static int uvc_start_transport(const struct uvc_request *req)
{
    if (!camera_attached)
        return -USB_ERR_NODEV;

    for (unsigned int i = 0; i < UVC_FRAME_COUNT; ++i)
    {
        frame_pool[i].frame_buf = frame_buffer[i];
        frame_pool[i].frame_bufsize = req->format == USBH_VIDEO_FORMAT_UNCOMPRESSED ?
                                     (uint32_t)req->width * req->height * 2U : UVC_FRAME_BYTES;
    }
    if (!pool_created)
    {
        int ret = usbh_video_stream_create(frame_pool, UVC_FRAME_COUNT);
        if (ret < 0)
            return ret;
        /* Stream start resets the frame queues for each session. */
        pool_created = RT_TRUE;
    }

    capture_width = req->width;
    capture_height = req->height;
    capture_mode = req->mode;
    frames_received = 0;
    frames_dropped = 0;
    display_fps = 0;
    open_result = RT_EOK;
    camera_error = RT_EOK;
    capture_idle = RT_FALSE;
    capture_run = RT_TRUE;
    camera_state = UVC_STARTING;
    transport_starts++;
    USB_LOG_INFO("UVC start: %ux%u format=%s\r\n", req->width, req->height,
                 req->format == USBH_VIDEO_FORMAT_MJPEG ? "mjpeg" : "yuyv");
    usbh_video_stream_start(req->width, req->height, req->format);
    rt_sem_release(&frame_wake);
    return RT_EOK;
}

static void uvc_control_thread_entry(void *arg)
{
    (void)arg;
    struct uvc_request active = {0};
    rt_bool_t transport_started = RT_FALSE;
    unsigned int retry_shift = 0;
    unsigned int consecutive_failures = 0;
    rt_bool_t reprobe_pending = RT_FALSE;
    rt_bool_t settling = RT_FALSE;
    uint32_t retry_id = 0;
    uint32_t previous_frames = 0;
    rt_tick_t sample_tick = 0, start_tick = 0, stop_tick = 0, closed_tick = 0;

    for (;;)
    {
        rt_sem_take(&control_wake, rt_tick_from_millisecond(20));
        struct uvc_request req = uvc_read_request();
        rt_tick_t now = rt_tick_get();

        if (!transport_started)
        {
            if (req.mode == UVC_MODE_OFF)
            {
                camera_state = camera_error == RT_EOK ? UVC_IDLE : UVC_FAILED;
                continue;
            }
            if (retry_id != req.id)
            {
                retry_id = req.id;
                retry_shift = 0;
                consecutive_failures = 0;
            }
            /* Give the device time to settle between closing the endpoint
             * and opening it again with SET_INTERFACE. */
            if (settling && !uvc_elapsed(now, closed_tick, UVC_REOPEN_QUIET_MS << retry_shift))
            {
                camera_state = UVC_STARTING;
                continue;
            }
            settling = RT_FALSE;
            if (!camera_attached)
            {
                camera_state = UVC_FAILED;
                camera_error = -USB_ERR_NODEV;
                retry_shift = 0;
                continue;
            }
            int ret = uvc_start_transport(&req);
            if (ret < 0)
            {
                uvc_fail_request(req.id, ret);
                camera_state = UVC_FAILED;
                continue;
            }
            active = req;
            transport_started = RT_TRUE;
            start_tick = sample_tick = now;
            previous_frames = 0;
            continue;
        }

        if (!capture_run)
        {
            if (capture_idle && rt_thread_find("usbh_video") == RT_NULL)
            {
                usbh_video_stream_stop();
#ifdef UVC_GUARD_USB_CALLBACKS
                rt_base_t level = rt_hw_interrupt_disable();
                uvc_stream_endpoint = RT_NULL;
                uvc_stream_port = RT_NULL;
                rt_memset(uvc_callbacks, 0, sizeof(uvc_callbacks));
                rt_hw_interrupt_enable(level);
#endif
                transport_started = RT_FALSE;
                closed_tick = rt_tick_get();
                settling = RT_TRUE;
                if (reprobe_pending && req.mode != UVC_MODE_OFF &&
                    usbh_hub_reprobe_root_port(camera_port) == 0)
                    USB_LOG_WRN("USB camera port reset requested\r\n");
                reprobe_pending = RT_FALSE;
                display_fps = 0;
                USB_LOG_INFO("UVC stopped\r\n");
            }
            else if (camera_state == UVC_STOPPING && uvc_elapsed(now, stop_tick, UVC_STOP_MS))
            {
                uvc_fail_request(req.id, -RT_ETIMEOUT);
                camera_error = -RT_ETIMEOUT;
                camera_state = UVC_FAILED;
                USB_LOG_ERR("UVC shutdown timed out; waiting for worker ownership to return\r\n");
            }
            continue;
        }

        rt_bool_t changed = req.width != active.width || req.height != active.height ||
                            req.format != active.format;
        int error = open_result;
        if (error >= 0 && frames_received == 0U && uvc_elapsed(now, start_tick, UVC_FIRST_FRAME_MS))
            error = -RT_ETIMEOUT;
        if (error >= 0 && frames_received != 0U && uvc_elapsed(now, frame_tick, UVC_NO_FRAME_MS))
            error = -RT_ETIMEOUT;

        if (req.mode == UVC_MODE_OFF || changed || error < 0)
        {
            if (error < 0 && req.mode != UVC_MODE_OFF && !changed)
            {
                if (!camera_attached)
                {
                    camera_error = error;
                }
                else if (uvc_retryable_error(error))
                {
                    camera_error = error;
                    if (retry_shift < UVC_REOPEN_MAX_SHIFT)
                        retry_shift++;
                    if (++consecutive_failures >= UVC_FAILURES_BEFORE_REPROBE)
                    {
                        reprobe_pending = RT_TRUE;
                        consecutive_failures = 0;
                    }
                    USB_LOG_WRN("UVC transport interrupted (%d), retry after %u ms\r\n",
                                 error, (unsigned int)(UVC_REOPEN_QUIET_MS << retry_shift));
                }
                else
                {
                    uvc_fail_request(req.id, error);
                    USB_LOG_ERR("UVC stream failed (%d), frames:%lu\r\n", error,
                                (unsigned long)frames_received);
                }
            }
            capture_run = RT_FALSE;
            camera_state = UVC_STOPPING;
            stop_tick = now;
            continue;
        }

        capture_mode = req.mode;
        if (frames_received != 0U)
        {
            camera_state = UVC_STREAMING;
            if (uvc_elapsed(now, start_tick, 8000))
            {
                retry_shift = 0;
                consecutive_failures = 0;
            }
        }
        if (uvc_elapsed(now, sample_tick, 1000))
        {
            uint32_t elapsed = (rt_tick_t)(now - sample_tick);
            uint32_t count = frames_received;
            display_fps = (count - previous_frames) * RT_TICK_PER_SECOND / elapsed;
            previous_frames = count;
            sample_tick = now;
        }
    }
}

static int uvc_control_init(void)
{
    rt_sem_init(&control_wake, "cam_ctl", 0, RT_IPC_FLAG_PRIO);
    rt_sem_init(&frame_wake, "cam_frm", 0, RT_IPC_FLAG_PRIO);
    rt_thread_init(&control_thread, "uvc_ctl", uvc_control_thread_entry, RT_NULL,
                   control_stack, sizeof(control_stack), 19, 10);
    rt_thread_init(&frame_thread, "uvc_frm", uvc_frame_thread_entry, RT_NULL,
                   frame_stack, sizeof(frame_stack), 22, 10);
    control_ready = RT_TRUE;
    rt_thread_startup(&frame_thread);
    rt_thread_startup(&control_thread);
    return RT_EOK;
}
INIT_APP_EXPORT(uvc_control_init);

static int uvc_request_mode(uint8_t fmt, uint16_t w, uint16_t h, enum uvc_mode mode, rt_bool_t reuse)
{
    if (!control_ready)
        return -RT_ERROR;
    if (mode == UVC_MODE_MONITOR)
        uvc_ai_app_enforce_mode(&fmt, &w, &h);
    if (w == 0U || h == 0U || w > 640U || h > 480U ||
        (fmt != USBH_VIDEO_FORMAT_MJPEG &&
         (fmt != USBH_VIDEO_FORMAT_UNCOMPRESSED || (uint32_t)w * h * 2U > UVC_FRAME_BYTES)))
        return -RT_EINVAL;

    rt_base_t level = rt_hw_interrupt_disable();
    if (!reuse && requested.mode != UVC_MODE_OFF)
    {
        rt_hw_interrupt_enable(level);
        return -RT_EBUSY;
    }
    requested.id++;
    requested.mode = mode;
    requested.format = fmt;
    requested.width = w;
    requested.height = h;
    camera_error = RT_EOK;
    rt_hw_interrupt_enable(level);
    rt_sem_release(&control_wake);
    return RT_EOK;
}

int uvc_app_start(uint8_t fmt, uint16_t w, uint16_t h)
{
    return uvc_request_mode(fmt, w, h, UVC_MODE_MONITOR, RT_FALSE);
}

int uvc_app_start_preview(void)
{
    return uvc_request_mode(USBH_VIDEO_FORMAT_UNCOMPRESSED, 320, 240, UVC_MODE_PREVIEW, RT_TRUE);
}

int uvc_app_start_monitor(void)
{
    return uvc_request_mode(USBH_VIDEO_FORMAT_UNCOMPRESSED, 320, 240, UVC_MODE_MONITOR, RT_TRUE);
}

int uvc_app_stop(void)
{
    if (!control_ready)
        return -RT_ERROR;
    rt_base_t level = rt_hw_interrupt_disable();
    requested.id++;
    requested.mode = UVC_MODE_OFF;
    camera_error = RT_EOK;
    rt_hw_interrupt_enable(level);
    rt_sem_release(&control_wake);
    return RT_EOK;
}

rt_bool_t uvc_app_is_running(void)
{
    return uvc_read_request().mode != UVC_MODE_OFF;
}

rt_bool_t uvc_app_is_preview(void)
{
    return uvc_read_request().mode == UVC_MODE_PREVIEW;
}

rt_bool_t uvc_app_has_frame(void)
{
    return capture_run && frames_received != 0U &&
           !uvc_elapsed(rt_tick_get(), frame_tick, UVC_NO_FRAME_MS);
}

rt_bool_t uvc_app_preview_frame_ready(void)
{
    return uvc_app_is_preview() && uvc_app_has_frame();
}

const char *uvc_app_camera_state(void)
{
    static const char *const names[] = { "idle", "starting", "streaming", "stopping", "error" };
    return names[camera_state];
}

uint32_t uvc_app_frame_count(void)
{
    return frames_received;
}

void uvc_app_get_fps(uint32_t *cam_fps, uint32_t *disp_fps, uint32_t *drop_cnt)
{
    rt_bool_t frame = uvc_app_has_frame();
    if (cam_fps) *cam_fps = frame ? g_uvc_fps : 0U;
    if (disp_fps) *disp_fps = frame ? display_fps : 0U;
    if (drop_cnt) *drop_cnt = frames_dropped;
}

static int cmd_uvc_status(void)
{
    uint32_t cam_fps, disp_fps;
    uvc_app_get_fps(&cam_fps, &disp_fps, RT_NULL);
    rt_kprintf("UVC state=%s attached=%d mode=%u error=%d frames=%lu cam_fps=%lu disp_fps=%lu dropped=%lu urb=%lu pool=%u starts=%lu suppressed=%lu recovered=%lu\n",
               uvc_app_camera_state(), camera_attached, (unsigned)uvc_read_request().mode, camera_error,
               (unsigned long)frames_received, (unsigned long)cam_fps, (unsigned long)disp_fps,
               (unsigned long)frames_dropped, (unsigned long)video_complete_count,
               (unsigned)pool_created, (unsigned long)transport_starts, (unsigned long)suppressed_urb_callbacks,
               (unsigned long)recovered_urb_errors);
    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_uvc_status, uvc_status, Show camera lifecycle and frame counters);

static int cmd_usbh_uvc_start(int argc, char **argv)
{
    uint8_t fmt = USBH_VIDEO_FORMAT_UNCOMPRESSED;
    uint16_t w = 320;
    uint16_t h = 240;
    if (argc >= 2) fmt = (uint8_t)atoi(argv[1]);
    if (argc >= 4)
    {
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
