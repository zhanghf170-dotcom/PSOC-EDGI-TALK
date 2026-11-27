#include <rtthread.h>
#include <string.h>
#include "usbh_uvc_stream.h"

#define UVC_MAX_FRAMES 8U
#define UVC_ISO_RETRY_LIMIT 3U
#define UVC_PAYLOAD_FID 0x01U
#define UVC_PAYLOAD_EOF 0x02U
#define UVC_PAYLOAD_ERR 0x40U

struct uvc_transfer
{
    struct usbh_urb urb;
    struct usbh_iso_frame_packet packets[VIDEO_ISO_PACKETS];
};

_Static_assert(offsetof(struct uvc_transfer, packets) == offsetof(struct usbh_urb, iso_packet),
               "ISO packets must immediately follow the URB");

static USB_MEM_ALIGNX uint8_t payload[2][VIDEO_ISO_PACKETS][VIDEO_EP_MPS]
    __attribute__((section(".cy_uvc_stream_data")));
static struct uvc_transfer transfers[2];
static struct rt_mailbox ready_frames;
static rt_ubase_t ready_storage[UVC_MAX_FRAMES];
static struct usbh_videoframe *pool;
static uint32_t pool_count;
static volatile uint32_t available;
static struct usbh_videoframe *assembling;
static uint32_t assembled_size;
static uint32_t expected_size;
static uint16_t stream_width, stream_height;
static uint8_t stream_format;
static uint8_t current_fid;
static bool have_fid, discard_frame, frame_closed;
static volatile bool accepting;
static uint8_t retries;
static struct usbh_video *opened_camera;

volatile uint32_t video_complete_count;
volatile uint32_t video_frame_count;
volatile uint32_t video_bad_packet_count;
volatile uint32_t video_dropped_frame_count;
volatile uint32_t video_recovered_urb_count;
volatile uint32_t video_suppressed_urb_count;

int usbh_video_stream_create(struct usbh_videoframe *frames, uint32_t count)
{
    if (!frames || count == 0U || count > UVC_MAX_FRAMES)
        return -USB_ERR_INVAL;

    pool = frames;
    pool_count = count;
    return rt_mb_init(&ready_frames, "uvc_rx", ready_storage, count, RT_IPC_FLAG_FIFO);
}

int usbh_video_stream_enqueue(struct usbh_videoframe *frame)
{
    rt_base_t level = rt_hw_interrupt_disable();
    available |= 1UL << (frame - pool);
    rt_hw_interrupt_enable(level);
    return 0;
}

int usbh_video_stream_dequeue(struct usbh_videoframe **frame, uint32_t timeout)
{
    rt_ubase_t value;
    int ret = rt_mb_recv(&ready_frames, &value, rt_tick_from_millisecond(timeout));
    if (ret == RT_EOK)
        *frame = (struct usbh_videoframe *)value;
    return ret;
}

static void finish_frame(void)
{
    if (assembling)
    {
        bool complete = (stream_format == USBH_VIDEO_FORMAT_UNCOMPRESSED) ?
                        assembled_size == expected_size :
                        assembled_size >= 4U &&
                        assembling->frame_buf[0] == 0xffU && assembling->frame_buf[1] == 0xd8U &&
                        assembling->frame_buf[assembled_size - 2U] == 0xffU &&
                        assembling->frame_buf[assembled_size - 1U] == 0xd9U;
        if (!discard_frame && complete)
        {
            assembling->frame_size = assembled_size;
            assembling->frame_format = stream_format;
            if (rt_mb_send(&ready_frames, (rt_ubase_t)assembling) == RT_EOK)
                video_frame_count++;
            else
                usbh_video_stream_enqueue(assembling);
        }
        else
        {
            video_dropped_frame_count++;
            usbh_video_stream_enqueue(assembling);
        }
        assembling = RT_NULL;
    }
    assembled_size = 0U;
    frame_closed = true;
}

static void append_packet(const struct usbh_iso_frame_packet *packet)
{
    uint32_t length = packet->actual_length;
    const uint8_t *data = packet->transfer_buffer;
    if (packet->errorcode < 0 || (length != 0U &&
        (length < 2U || length > packet->transfer_buffer_length ||
         data[0] < 2U || data[0] > length)))
    {
        video_bad_packet_count++;
        discard_frame = true;
        return;
    }
    if (length == 0U)
        return;

    uint8_t flags = data[1];
    uint8_t fid = flags & UVC_PAYLOAD_FID;
    uint8_t header_size = data[0];
    if (!have_fid)
    {
        /* USB may start in the middle of a frame. Wait for a clean boundary. */
        current_fid = fid;
        have_fid = true;
        discard_frame = true;
    }
    else if (fid != current_fid)
    {
        finish_frame();
        current_fid = fid;
        discard_frame = false;
        frame_closed = false;
    }
    if (frame_closed)
        return;

    if (flags & UVC_PAYLOAD_ERR)
    {
        video_bad_packet_count++;
        discard_frame = true;
    }
    length -= header_size;
    if (!discard_frame && length != 0U)
    {
        if (!assembling)
        {
            uint32_t mask = available;
            if (mask != 0U)
            {
                unsigned int slot = (unsigned int)__builtin_ctz(mask);
                available &= ~(1UL << slot);
                assembling = &pool[slot];
            }
            else
            {
                discard_frame = true;
                video_dropped_frame_count++;
            }
        }

        if (assembling && length <= assembling->frame_bufsize - assembled_size)
        {
            memcpy(assembling->frame_buf + assembled_size, data + header_size, length);
            assembled_size += length;
        }
        else
        {
            discard_frame = true;
        }
    }
    if (flags & UVC_PAYLOAD_EOF)
        finish_frame();
}

static void uvc_iso_complete(void *arg, int status)
{
    const unsigned int slot = (unsigned int)(uintptr_t)arg;
    if (!accepting)
    {
        video_suppressed_urb_count++;
        return;
    }

    struct uvc_transfer *next = &transfers[slot ^ 1U];
    for (unsigned int i = 0U; i < VIDEO_ISO_PACKETS; i++)
    {
        next->packets[i].actual_length = 0U;
        next->packets[i].errorcode = 0;
    }

    if (status < 0 && (status != -USB_ERR_IO || retries >= UVC_ISO_RETRY_LIMIT))
    {
        usbh_video_transfer_abort_callback();
        return;
    }
    if (usbh_submit_urb(&next->urb) != 0)
    {
        usbh_video_transfer_abort_callback();
        return;
    }
    if (status < 0)
    {
        retries++;
        video_recovered_urb_count++;
        discard_frame = true;
        return;
    }
    retries = 0U;
    const struct uvc_transfer *done = &transfers[slot];
    for (unsigned int i = 0U; i < VIDEO_ISO_PACKETS; i++)
        append_packet(&done->packets[i]);
    video_complete_count++;
}

static void stream_open(void *arg)
{
    (void)arg;
    struct usbh_video *video = usbh_find_class_instance("/dev/video0");
    if (!video || !video->hport || video->is_bulk)
    {
        usbh_video_stream_open_result(video, video ? -USB_ERR_NOTSUPP : -USB_ERR_NODEV);
        return;
    }

    uint8_t alt = 0U;
    uint16_t best_mps = 0U;
    for (uint8_t i = 1U; i < video->num_of_intf_altsettings; i++)
    {
        const struct usb_endpoint_descriptor *ep =
            &video->hport->config.intf[video->data_intf].altsetting[i].ep[0].ep_desc;
        uint16_t size = USB_GET_MAXPACKETSIZE(ep->wMaxPacketSize) * (USB_GET_MULT(ep->wMaxPacketSize) + 1U);
        if ((ep->bEndpointAddress & USB_ENDPOINT_DIRECTION_MASK) &&
            (ep->bmAttributes & USB_ENDPOINT_TYPE_MASK) == USB_ENDPOINT_TYPE_ISOCHRONOUS &&
            size > best_mps && size <= VIDEO_EP_MPS)
        {
            best_mps = size;
            alt = i;
        }
    }
    if (alt == 0U)
    {
        usbh_video_stream_open_result(video, -USB_ERR_NOTSUPP);
        return;
    }

    if (usbh_video_open(video, stream_format, stream_width, stream_height, alt) < 0)
        return;
    opened_camera = video;
    if (!accepting || !video->hport || !video->is_opened)
        return;

    for (unsigned int slot = 0U; slot < 2U; slot++)
    {
        struct uvc_transfer *transfer = &transfers[slot];
        memset(transfer, 0, sizeof(*transfer));
        transfer->urb.hport = video->hport;
        transfer->urb.ep = video->isoin;
        transfer->urb.complete = uvc_iso_complete;
        transfer->urb.arg = (void *)(uintptr_t)slot;
        transfer->urb.num_of_iso_packets = VIDEO_ISO_PACKETS;
        for (unsigned int i = 0U; i < VIDEO_ISO_PACKETS; i++)
        {
            transfer->packets[i].transfer_buffer = payload[slot][i];
            transfer->packets[i].transfer_buffer_length = video->isoin_mps;
        }
    }

    rt_base_t level = rt_hw_interrupt_disable();
    int ret = accepting ? usbh_submit_urb(&transfers[0].urb) : 0;
    rt_hw_interrupt_enable(level);
    if (ret < 0)
        usbh_video_stream_open_result(video, ret);
}

void usbh_video_stream_start(uint16_t width, uint16_t height, uint8_t format)
{
    stream_width = width;
    stream_height = height;
    stream_format = format;
    expected_size = (uint32_t)width * height * 2U;
    rt_mb_control(&ready_frames, RT_IPC_CMD_RESET, RT_NULL);
    available = (1UL << pool_count) - 1UL;
    assembling = RT_NULL;
    assembled_size = 0U;
    video_complete_count = 0;
    video_frame_count = 0;
    have_fid = discard_frame = frame_closed = false;
    retries = 0U;
    accepting = true;

    rt_thread_t thread = rt_thread_create("usbh_video", stream_open, RT_NULL, 3072, 12, 10);
    if (!thread)
    {
        usbh_video_stream_open_result(RT_NULL, -USB_ERR_NOMEM);
        return;
    }
    rt_thread_startup(thread);
}

void usbh_video_stream_cancel(void)
{
    accepting = false;
}

void usbh_video_stream_stop(void)
{
    usbh_video_stream_cancel();
    for (unsigned int i = 0U; i < 2U; i++)
    {
        struct usbh_urb *urb = &transfers[i].urb;
        if (urb->hcpriv)
            usbh_kill_urb(urb);
    }
    if (opened_camera && opened_camera == usbh_find_class_instance("/dev/video0") &&
        opened_camera->is_opened)
        usbh_video_close(opened_camera);
    opened_camera = RT_NULL;
    video_complete_count = 0;
}

void usbh_video_stream_get_info(uint16_t *width, uint16_t *height, uint8_t *format)
{
    if (width) *width = stream_width;
    if (height) *height = stream_height;
    if (format) *format = stream_format;
}
