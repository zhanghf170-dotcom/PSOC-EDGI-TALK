/*
 * Copyright (c) 2022, sakumisu
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "usbh_core.h"
#include "usbh_video.h"

#undef USB_DBG_TAG
#define USB_DBG_TAG "usbh_video"
#include "usb_log.h"

#define DEV_FORMAT "/dev/video%d"

/* general descriptor field offsets */
#define DESC_bLength              0 /** Length offset */
#define DESC_bDescriptorType      1 /** Descriptor type offset */
#define DESC_bDescriptorSubType   2 /** Descriptor subtype offset */
#define DESC_bNumFormats          3 /** Descriptor numformat offset */
#define DESC_bNumFrameDescriptors 4 /** Descriptor numframe offset */
#define DESC_bFormatIndex         3 /** Descriptor format index offset */
#define DESC_bFrameIndex          3 /** Descriptor frame index offset */

/* interface descriptor field offsets */
#define INTF_DESC_bInterfaceNumber  2 /** Interface number offset */
#define INTF_DESC_bAlternateSetting 3 /** Alternate setting offset */

USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t g_video_buf[USB_ALIGN_UP(128, CONFIG_USB_ALIGN_SIZE)];

static const char *format_type[] = { "uncompressed", "mjpeg" };

static struct usbh_video g_video_class[CONFIG_USBHOST_MAX_VIDEO_CLASS];
static uint32_t g_devinuse = 0;

static struct usbh_video *usbh_video_class_alloc(void)
{
    uint8_t devno;

    for (devno = 0; devno < CONFIG_USBHOST_MAX_VIDEO_CLASS; devno++) {
        if ((g_devinuse & (1U << devno)) == 0) {
            g_devinuse |= (1U << devno);
            memset(&g_video_class[devno], 0, sizeof(struct usbh_video));
            g_video_class[devno].minor = devno;
            return &g_video_class[devno];
        }
    }
    return NULL;
}

static void usbh_video_class_free(struct usbh_video *video_class)
{
    uint8_t devno = video_class->minor;

    if (devno < 32) {
        g_devinuse &= ~(1U << devno);
    }
    memset(video_class, 0, sizeof(struct usbh_video));
}

int usbh_video_get(struct usbh_video *video_class, uint8_t request, uint8_t intf, uint8_t entity_id, uint8_t cs, uint8_t *buf, uint16_t len)
{
    struct usb_setup_packet *setup;
    int ret;

    if (!video_class || !video_class->hport || !video_class->hport->connected ||
        len > sizeof(g_video_buf)) {
        return -USB_ERR_INVAL;
    }
    setup = video_class->hport->setup;

    setup->bmRequestType = USB_REQUEST_DIR_IN | USB_REQUEST_CLASS | USB_REQUEST_RECIPIENT_INTERFACE;
    setup->bRequest = request;
    setup->wValue = cs << 8;
    setup->wIndex = (entity_id << 8) | intf;
    setup->wLength = len;

    ret = usbh_control_transfer(video_class->hport, setup, g_video_buf);
    if (buf && ret > 0)
        memcpy(buf, g_video_buf, (uint16_t)ret < len ? (uint16_t)ret : len);

    return ret;
}

int usbh_video_set(struct usbh_video *video_class, uint8_t request, uint8_t intf, uint8_t entity_id, uint8_t cs, uint8_t *buf, uint16_t len)
{
    struct usb_setup_packet *setup;
    int ret;

    if (!video_class || !video_class->hport || !video_class->hport->connected ||
        !buf || len > sizeof(g_video_buf)) {
        return -USB_ERR_INVAL;
    }
    setup = video_class->hport->setup;

    setup->bmRequestType = USB_REQUEST_DIR_OUT | USB_REQUEST_CLASS | USB_REQUEST_RECIPIENT_INTERFACE;
    setup->bRequest = request;
    setup->wValue = cs << 8;
    setup->wIndex = (entity_id << 8) | intf;
    setup->wLength = len;

    memcpy(g_video_buf, buf, len);

    ret = usbh_control_transfer(video_class->hport, setup, g_video_buf);
    if (ret >= 0)
        usb_osal_msleep(15);
    return ret;
}

static uint16_t video_probe_length(const struct usbh_video *video_class)
{
    if (video_class->bcdVDC >= 0x0150)
        return sizeof(video_class->probe);
    return video_class->bcdVDC >= 0x0110 ? 34U : 26U;
}

static int video_get_probe(struct usbh_video *video_class, uint16_t length)
{
    memset(&video_class->probe, 0, sizeof(video_class->probe));
    return usbh_video_get(video_class, VIDEO_REQUEST_GET_CUR, video_class->data_intf, 0,
                          VIDEO_VS_PROBE_CONTROL, (uint8_t *)&video_class->probe, length);
}

/* Choose from the clock intervals advertised for this frame size. */
static uint32_t video_monitor_interval(const uint8_t *desc, uint32_t default_interval)
{
    const uint32_t target = 666667U; /* 15 FPS, in 100 ns units */
    const uint8_t len = desc[0];
    if (len < 26U)
        return default_interval;

    uint8_t count = desc[25];
    if (count == 0U && len >= 38U) {
        uint32_t min_interval, max_interval, step;
        memcpy(&min_interval, desc + 26U, sizeof(min_interval));
        memcpy(&max_interval, desc + 30U, sizeof(max_interval));
        memcpy(&step, desc + 34U, sizeof(step));
        if (min_interval > 0U && min_interval <= max_interval) {
            uint32_t requested = target < min_interval ? min_interval :
                                 (target > max_interval ? max_interval : target);
            return step ? min_interval + ((requested - min_interval) / step) * step : requested;
        }
    }

    uint32_t chosen = default_interval;
    uint32_t best = (chosen > target) ? chosen - target : target - chosen;
    for (uint8_t i = 0U; i < count && 30U + 4U * i <= len; i++) {
        uint32_t interval;
        memcpy(&interval, desc + 26U + 4U * i, sizeof(interval));
        uint32_t diff = (interval > target) ? interval - target : target - interval;
        if (interval != 0U && diff < best) {
            best = diff;
            chosen = interval;
        }
    }
    return chosen;
}

static void video_keep_frame_rate(struct usbh_video *video)
{
    if (video->ae_priority_terminal == 0U)
        return;

    uint8_t priority = 0U;
    int ret = usbh_video_get(video, VIDEO_REQUEST_GET_CUR, video->ctrl_intf,
                             video->ae_priority_terminal, VIDEO_CT_AE_PRIORITY_CONTROL,
                             &priority, sizeof(priority));
    if (ret == sizeof(priority) && priority == 1U) {
        priority = 0U;
        ret = usbh_video_set(video, VIDEO_REQUEST_SET_CUR, video->ctrl_intf,
                             video->ae_priority_terminal, VIDEO_CT_AE_PRIORITY_CONTROL,
                             &priority, sizeof(priority));
        if (ret >= 0)
            USB_LOG_INFO("Video fixed frame rate enabled\r\n");
    }
}

int usbh_video_open(struct usbh_video *video_class,
                    uint8_t format_type,
                    uint16_t wWidth,
                    uint16_t wHeight,
                    uint8_t altsetting)
{
    struct usb_setup_packet *setup;
    struct usb_endpoint_descriptor *ep_desc;
    uint8_t mult;
    uint16_t mps;
    int ret;
    bool found = false;
    uint8_t formatidx = 0;
    uint8_t frameidx = 0;
    uint32_t dwDefaultFrameInterval = 0;
    const char *step;

    if (!video_class || !video_class->hport || !video_class->hport->connected) {
        usbh_video_stream_open_result(video_class, -USB_ERR_NODEV);
        return -USB_ERR_INVAL;
    }
    setup = video_class->hport->setup;

    if (video_class->is_opened) {
        usbh_video_stream_open_result(video_class, 0);
        return 0;
    }

    for (uint8_t i = 0; i < video_class->num_of_formats; i++) {
        if (format_type == video_class->format[i].format_type) {
            formatidx = i + 1;
            for (uint8_t j = 0; j < video_class->format[i].num_of_frames; j++) {
                if ((wWidth == video_class->format[i].frame[j].wWidth) &&
                    (wHeight == video_class->format[i].frame[j].wHeight)) {
                    frameidx = j + 1;
                    const struct usbh_video_resolution *frame = &video_class->format[i].frame[j];
                    dwDefaultFrameInterval = (wWidth == 320U && wHeight == 240U &&
                                              format_type == USBH_VIDEO_FORMAT_UNCOMPRESSED &&
                                              frame->dwMonitorFrameInterval != 0U) ?
                                             frame->dwMonitorFrameInterval : frame->dwDefaultFrameInterval;
                    found = true;
                    break;
                }
            }
        }
    }

    if (found == false) {
        usbh_video_stream_open_result(video_class, -USB_ERR_NODEV);
        return -USB_ERR_NODEV;
    }

    if (altsetting > (video_class->num_of_intf_altsettings - 1)) {
        usbh_video_stream_open_result(video_class, -USB_ERR_INVAL);
        return -USB_ERR_INVAL;
    }

    uint16_t length = video_probe_length(video_class);
    ret = video_get_probe(video_class, length);
    if (ret == 26 || ret == 34)
        length = (uint16_t)ret;
    video_class->probe.hintUnion.bmHint = 1;
    video_class->probe.bFormatIndex = formatidx;
    video_class->probe.bFrameIndex = frameidx;
    video_class->probe.dwFrameInterval = dwDefaultFrameInterval;
    video_class->probe.dwMaxPayloadTransferSize = 0;
    step = "SET_CUR(PROBE)";
    ret = usbh_video_set(video_class, VIDEO_REQUEST_SET_CUR, video_class->data_intf, 0,
                         VIDEO_VS_PROBE_CONTROL, (uint8_t *)&video_class->probe, length);
    if (ret < 0) {
        goto errout;
    }

    step = "GET_CUR(PROBE)";
    ret = video_get_probe(video_class, length);
    if (ret < 26) {
        if (ret >= 0)
            ret = -USB_ERR_IO;
        goto errout;
    }
    if (video_class->probe.bFormatIndex != formatidx || video_class->probe.bFrameIndex != frameidx) {
        ret = -USB_ERR_INVAL;
        goto errout;
    }

    step = "SET_CUR(COMMIT)";
    video_class->commit = video_class->probe;
    ret = usbh_video_set(video_class, VIDEO_REQUEST_SET_CUR, video_class->data_intf, 0,
                         VIDEO_VS_COMMIT_CONTROL, (uint8_t *)&video_class->commit, length);
    if (ret < 0) {
        goto errout;
    }

    if (format_type == USBH_VIDEO_FORMAT_UNCOMPRESSED && wWidth == 320U && wHeight == 240U)
        video_keep_frame_rate(video_class);

    step = "SET_INTERFACE";
    if (!video_class->is_bulk) {
        setup->bmRequestType = USB_REQUEST_DIR_OUT | USB_REQUEST_STANDARD | USB_REQUEST_RECIPIENT_INTERFACE;
        setup->bRequest = USB_REQUEST_SET_INTERFACE;
        setup->wValue = altsetting;
        setup->wIndex = video_class->data_intf;
        setup->wLength = 0;

        ret = usbh_control_transfer(video_class->hport, setup, NULL);
        if (ret < 0) {
            goto errout;
        }

        ep_desc = &video_class->hport->config.intf[video_class->data_intf].altsetting[altsetting].ep[0].ep_desc;
        mult = (ep_desc->wMaxPacketSize & USB_MAXPACKETSIZE_ADDITIONAL_TRANSCATION_MASK) >> USB_MAXPACKETSIZE_ADDITIONAL_TRANSCATION_SHIFT;
        mps = ep_desc->wMaxPacketSize & USB_MAXPACKETSIZE_MASK;
        if (ep_desc->bEndpointAddress & 0x80) {
            video_class->isoin_mps = mps * (mult + 1);
            USBH_EP_INIT(video_class->isoin, ep_desc);
        } else {
            ret = -USB_ERR_NODEV;
            goto errout;
        }
    } else {
        ep_desc = &video_class->hport->config.intf[video_class->data_intf].altsetting[0].ep[0].ep_desc;
        USBH_EP_INIT(video_class->bulkin, ep_desc);
    }

    USB_LOG_INFO("Open video formatidx:%u frameidx:%u altsetting:%u interval:%lu\r\n",
                 formatidx, frameidx, altsetting, (unsigned long)video_class->commit.dwFrameInterval);
    video_class->is_opened = true;
    video_class->current_format = format_type;
    usbh_video_stream_open_result(video_class, ret);
    return ret;

errout:
    USB_LOG_ERR("Video %ux%u UVC %04x %s len:%u failed (%d)\r\n",
                wWidth, wHeight, video_class->bcdVDC, step, length, ret);
    usbh_video_stream_open_result(video_class, ret);
    return ret;
}

int usbh_video_close(struct usbh_video *video_class)
{
    struct usb_setup_packet *setup;
    int ret = 0;

    if (!video_class || !video_class->hport) {
        return -USB_ERR_INVAL;
    }
    setup = video_class->hport->setup;

    USB_LOG_INFO("Close video device\r\n");

    video_class->is_opened = false;

    if (video_class->is_bulk) {
        setup->bmRequestType = USB_REQUEST_DIR_OUT | USB_REQUEST_STANDARD | USB_REQUEST_RECIPIENT_ENDPOINT;
        setup->bRequest = USB_REQUEST_CLEAR_FEATURE;
        setup->wValue = USB_FEATURE_ENDPOINT_HALT;
        setup->wIndex = video_class->bulkin->bEndpointAddress;
        setup->wLength = 0;
    } else {
        setup->bmRequestType = USB_REQUEST_DIR_OUT | USB_REQUEST_STANDARD | USB_REQUEST_RECIPIENT_INTERFACE;
        setup->bRequest = USB_REQUEST_SET_INTERFACE;
        setup->wValue = 0;
        setup->wIndex = video_class->data_intf;
        setup->wLength = 0;
    }

    ret = usbh_control_transfer(video_class->hport, setup, NULL);
    if (ret < 0) {
        return ret;
    }

    return ret;
}

void usbh_video_list_info(struct usbh_video *video_class)
{
    struct usb_endpoint_descriptor *ep_desc;

    USB_LOG_INFO("============= Video device information ===================\r\n");
    USB_LOG_RAW("bcdVDC:%04x\r\n", video_class->bcdVDC);
    USB_LOG_RAW("Num of altsettings:%u (%s mode)\r\n", video_class->num_of_intf_altsettings, video_class->num_of_intf_altsettings == 1 ? "bulk" : "iso");

    video_class->is_bulk = video_class->num_of_intf_altsettings == 1 ? true : false;

    if (video_class->is_bulk) {
        ep_desc = &video_class->hport->config.intf[video_class->data_intf].altsetting[0].ep[0].ep_desc;
        USB_LOG_RAW("Ep=%02x Attr=%02u Mps=%d Interval=%02u Mult=%02u\r\n",
                    ep_desc->bEndpointAddress,
                    ep_desc->bmAttributes,
                    USB_GET_MAXPACKETSIZE(ep_desc->wMaxPacketSize),
                    ep_desc->bInterval,
                    USB_GET_MULT(ep_desc->wMaxPacketSize));
    } else {
        for (uint8_t i = 0; i < video_class->num_of_intf_altsettings; i++) {
            if (i == 0) {
                USB_LOG_RAW("Ingore altsetting 0\r\n");
                continue;
            }
            ep_desc = &video_class->hport->config.intf[video_class->data_intf].altsetting[i].ep[0].ep_desc;

            USB_LOG_RAW("Altsetting:%u, Ep=%02x Attr=%02u Mps=%d Interval=%02u Mult=%02u\r\n",
                        i,
                        ep_desc->bEndpointAddress,
                        ep_desc->bmAttributes,
                        USB_GET_MAXPACKETSIZE(ep_desc->wMaxPacketSize),
                        ep_desc->bInterval,
                        USB_GET_MULT(ep_desc->wMaxPacketSize));
        }
    }

    USB_LOG_RAW("bNumFormats:%u\r\n", video_class->num_of_formats);
    for (uint8_t i = 0; i < video_class->num_of_formats; i++) {
        USB_LOG_RAW("  FormatIndex:%u\r\n", i + 1);
        USB_LOG_RAW("  FormatType:%s\r\n", format_type[video_class->format[i].format_type]);
        USB_LOG_RAW("  bNumFrames:%u\r\n", video_class->format[i].num_of_frames);
        USB_LOG_RAW("  Resolution:\r\n");
        for (uint8_t j = 0; j < video_class->format[i].num_of_frames; j++) {
            USB_LOG_RAW("      FrameIndex:%u\r\n", j + 1);
            USB_LOG_RAW("      wWidth: %d, wHeight: %d, fps: %d\r\n",
                        video_class->format[i].frame[j].wWidth,
                        video_class->format[i].frame[j].wHeight,
                        (1000 / (video_class->format[i].frame[j].dwDefaultFrameInterval / 10000)));
        }
    }

    USB_LOG_INFO("============= Video device information ===================\r\n");
}

static int usbh_video_ctrl_connect(struct usbh_hubport *hport, uint8_t intf)
{
    uint8_t cur_iface = 0xff;
    // uint8_t cur_alt_setting = 0xff;
    uint8_t frame_index = 0xff;
    uint8_t format_index = 0xff;
    uint8_t num_of_frames = 0xff;
    uint8_t *p;

    struct usbh_video *video_class = usbh_video_class_alloc();
    if (video_class == NULL) {
        USB_LOG_ERR("Fail to alloc video_class\r\n");
        return -USB_ERR_NOMEM;
    }

    video_class->hport = hport;
    video_class->ctrl_intf = intf;
    video_class->data_intf = intf + 1;
    video_class->num_of_intf_altsettings = hport->config.intf[intf + 1].altsetting_num;

    hport->config.intf[intf].priv = video_class;

    p = hport->raw_config_desc;
    while (p[DESC_bLength]) {
        switch (p[DESC_bDescriptorType]) {
            case USB_DESCRIPTOR_TYPE_INTERFACE:
                cur_iface = p[INTF_DESC_bInterfaceNumber];
                //cur_alt_setting = p[INTF_DESC_bAlternateSetting];
                break;
            case USB_DESCRIPTOR_TYPE_ENDPOINT:
                //ep_desc = (struct usb_endpoint_descriptor *)p;
                break;
            case VIDEO_CS_INTERFACE_DESCRIPTOR_TYPE:
                if (cur_iface == video_class->ctrl_intf) {
                    switch (p[DESC_bDescriptorSubType]) {
                        case VIDEO_VC_HEADER_DESCRIPTOR_SUBTYPE:
                            video_class->bcdVDC = ((uint16_t)p[4] << 8) | (uint16_t)p[3];
                            break;
                        case VIDEO_VC_INPUT_TERMINAL_DESCRIPTOR_SUBTYPE:
                            if (p[DESC_bLength] >= 16U && p[4] == (VIDEO_ITT_CAMERA & 0xffU) &&
                                p[5] == (VIDEO_ITT_CAMERA >> 8U) && p[14] > 0U &&
                                (p[15] & (1U << 2U))) {
                                video_class->ae_priority_terminal = p[3];
                            }
                            break;
                        case VIDEO_VC_OUTPUT_TERMINAL_DESCRIPTOR_SUBTYPE:
                            break;
                        case VIDEO_VC_PROCESSING_UNIT_DESCRIPTOR_SUBTYPE:
                            break;

                        default:
                            break;
                    }
                } else if (cur_iface == video_class->data_intf) {
                    switch (p[DESC_bDescriptorSubType]) {
                        case VIDEO_VS_INPUT_HEADER_DESCRIPTOR_SUBTYPE:
                            video_class->num_of_formats = p[DESC_bNumFormats];
                            break;
                        case VIDEO_VS_FORMAT_UNCOMPRESSED_DESCRIPTOR_SUBTYPE:
                            format_index = p[DESC_bFormatIndex];
                            num_of_frames = p[DESC_bNumFrameDescriptors];

                            video_class->format[format_index - 1].num_of_frames = num_of_frames;
                            video_class->format[format_index - 1].format_type = USBH_VIDEO_FORMAT_UNCOMPRESSED;
                            break;
                        case VIDEO_VS_FORMAT_MJPEG_DESCRIPTOR_SUBTYPE:
                            format_index = p[DESC_bFormatIndex];
                            num_of_frames = p[DESC_bNumFrameDescriptors];

                            video_class->format[format_index - 1].num_of_frames = num_of_frames;
                            video_class->format[format_index - 1].format_type = USBH_VIDEO_FORMAT_MJPEG;
                            break;
                        case VIDEO_VS_FRAME_UNCOMPRESSED_DESCRIPTOR_SUBTYPE:
                            if (p[DESC_bLength] < 26U)
                                break;
                            frame_index = p[DESC_bFrameIndex];

                            video_class->format[format_index - 1].frame[frame_index - 1].wWidth = ((struct video_cs_if_vs_frame_uncompressed_descriptor *)p)->wWidth;
                            video_class->format[format_index - 1].frame[frame_index - 1].wHeight = ((struct video_cs_if_vs_frame_uncompressed_descriptor *)p)->wHeight;
                            video_class->format[format_index - 1].frame[frame_index - 1].dwDefaultFrameInterval = ((struct video_cs_if_vs_frame_uncompressed_descriptor *)p)->dwDefaultFrameInterval;
                            video_class->format[format_index - 1].frame[frame_index - 1].dwMonitorFrameInterval =
                                video_monitor_interval(p, ((struct video_cs_if_vs_frame_uncompressed_descriptor *)p)->dwDefaultFrameInterval);
                            break;
                        case VIDEO_VS_FRAME_MJPEG_DESCRIPTOR_SUBTYPE:
                            frame_index = p[DESC_bFrameIndex];

                            video_class->format[format_index - 1].frame[frame_index - 1].wWidth = ((struct video_cs_if_vs_frame_mjpeg_descriptor *)p)->wWidth;
                            video_class->format[format_index - 1].frame[frame_index - 1].wHeight = ((struct video_cs_if_vs_frame_mjpeg_descriptor *)p)->wHeight;
                            video_class->format[format_index - 1].frame[frame_index - 1].dwDefaultFrameInterval = ((struct video_cs_if_vs_frame_mjpeg_descriptor *)p)->dwDefaultFrameInterval;
                            break;
                        default:
                            break;
                    }
                }

                break;

            default:
                break;
        }
        /* skip to next descriptor */
        p += p[DESC_bLength];
    }

    usbh_video_list_info(video_class);

    snprintf(hport->config.intf[intf].devname, CONFIG_USBHOST_DEV_NAMELEN, DEV_FORMAT, video_class->minor);

    USB_LOG_INFO("Register Video Class:%s\r\n", hport->config.intf[intf].devname);

    usbh_video_run(video_class);
    return 0;
}

static int usbh_video_ctrl_disconnect(struct usbh_hubport *hport, uint8_t intf)
{
    int ret = 0;

    struct usbh_video *video_class = (struct usbh_video *)hport->config.intf[intf].priv;

    if (video_class) {
        if (hport->config.intf[intf].devname[0] != '\0') {
            usb_osal_thread_schedule_other();
            USB_LOG_INFO("Unregister Video Class:%s\r\n", hport->config.intf[intf].devname);
            usbh_video_stop(video_class);
        }

        usbh_video_class_free(video_class);
    }

    return ret;
}

static int usbh_video_streaming_connect(struct usbh_hubport *hport, uint8_t intf)
{
    (void)hport;
    (void)intf;
    return 0;
}

static int usbh_video_streaming_disconnect(struct usbh_hubport *hport, uint8_t intf)
{
    (void)hport;
    (void)intf;
    return 0;
}

__WEAK void usbh_video_run(struct usbh_video *video_class)
{
    (void)video_class;
}

__WEAK void usbh_video_stop(struct usbh_video *video_class)
{
    (void)video_class;
}

__WEAK void usbh_video_stream_open_result(const struct usbh_video *video_class, int result)
{
    (void)video_class;
    (void)result;
}

const struct usbh_class_driver video_ctrl_class_driver = {
    .driver_name = "video_ctrl",
    .connect = usbh_video_ctrl_connect,
    .disconnect = usbh_video_ctrl_disconnect
};

const struct usbh_class_driver video_streaming_class_driver = {
    .driver_name = "video_streaming",
    .connect = usbh_video_streaming_connect,
    .disconnect = usbh_video_streaming_disconnect
};

CLASS_INFO_DEFINE const struct usbh_class_info video_ctrl_class_info = {
    .match_flags = USB_CLASS_MATCH_INTF_CLASS | USB_CLASS_MATCH_INTF_SUBCLASS | USB_CLASS_MATCH_INTF_PROTOCOL,
    .bInterfaceClass = USB_DEVICE_CLASS_VIDEO,
    .bInterfaceSubClass = VIDEO_SC_VIDEOCONTROL,
    .bInterfaceProtocol = VIDEO_PC_PROTOCOL_UNDEFINED,
    .id_table = NULL,
    .class_driver = &video_ctrl_class_driver
};

CLASS_INFO_DEFINE const struct usbh_class_info video_streaming_class_info = {
    .match_flags = USB_CLASS_MATCH_INTF_CLASS | USB_CLASS_MATCH_INTF_SUBCLASS | USB_CLASS_MATCH_INTF_PROTOCOL,
    .bInterfaceClass = USB_DEVICE_CLASS_VIDEO,
    .bInterfaceSubClass = VIDEO_SC_VIDEOSTREAMING,
    .bInterfaceProtocol = VIDEO_PC_PROTOCOL_UNDEFINED,
    .id_table = NULL,
    .class_driver = &video_streaming_class_driver
};
