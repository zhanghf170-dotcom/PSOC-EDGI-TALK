/*
 * All Intellectual Property rights in the software belongs to sakumisu.
 *
 *   Licensing information
 *   ---------------------
 *
 *   Licensor:                 sakumisu
 *   Licensed to:
 *   License software version: cherryusb dwc2 host uvc&uac v2.1
 *   Licensed platform:
 */

#ifndef USBH_UVC_STREAM_H
#define USBH_UVC_STREAM_H

#include "usbh_core.h"
#include "usbh_video.h"

#define VIDEO_ISO_INTERVAL (2)
#define VIDEO_ISO_PACKETS  (8 * VIDEO_ISO_INTERVAL)
#define VIDEO_EP_MPS       3072

extern volatile uint32_t video_complete_count;
extern volatile uint32_t video_frame_count;
extern volatile uint32_t video_bad_packet_count;
extern volatile uint32_t video_dropped_frame_count;
extern volatile uint32_t video_recovered_urb_count;
extern volatile uint32_t video_suppressed_urb_count;

/* implemented by user */
void usbh_video_transfer_abort_callback(void);

int usbh_video_stream_create(struct usbh_videoframe *frame, uint32_t count);
void usbh_video_stream_start(uint16_t width, uint16_t height, uint8_t format);
void usbh_video_stream_cancel(void);
void usbh_video_stream_stop(void);
void usbh_video_stream_get_info(uint16_t *width, uint16_t *height, uint8_t *format);
int usbh_video_stream_enqueue(struct usbh_videoframe *frame);
int usbh_video_stream_dequeue(struct usbh_videoframe **frame, uint32_t timeout);

#endif
