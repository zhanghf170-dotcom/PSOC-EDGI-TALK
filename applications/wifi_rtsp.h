#ifndef WIFI_RTSP_H
#define WIFI_RTSP_H

#include <rtthread.h>
#include <stdint.h>

rt_bool_t wifi_rtsp_is_playing(void);
const uint8_t *wifi_stream_acquire_rtsp_jpeg(uint32_t *seq, size_t *len, int *slot);
void wifi_stream_release_rtsp_jpeg(int slot);

#endif
