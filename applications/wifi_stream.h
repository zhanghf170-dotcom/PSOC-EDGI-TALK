#ifndef WIFI_STREAM_H__
#define WIFI_STREAM_H__

#include <rtthread.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WIFI_STREAM_HTTP_PORT      8080
#define WIFI_STREAM_RTSP_PORT      8554
#define WIFI_STREAM_RELAY_PORT_DEF 8888
#define WIFI_MGR_MAX_SCAN_APS      16

typedef struct
{
    char     ssid[33];
    int16_t  rssi;
    uint8_t  channel;
    uint32_t security;
} wifi_ap_item_t;

typedef struct
{
    rt_bool_t whd_ready;
    rt_bool_t connected;
    rt_bool_t ready;
    rt_bool_t scanning;
    rt_bool_t connecting;
    rt_bool_t http_enabled;
    rt_bool_t relay_auto_push;
    rt_bool_t last_relay_ok;
    int       rssi;
    int       http_clients;
    int       jpeg_quality;
    int       max_fps;
    char      ssid[33];
    char      ip_str[20];
    char      relay_host[64];
    char      status_msg[48];
    uint16_t  relay_port;
    uint32_t  stream_fps;
    uint32_t  tx_kbps;
    uint32_t  frames_encoded;
    uint32_t  relay_sent_count;
} wifi_status_t;

int  wifi_stream_init(void);
void wifi_stream_submit_frame(const uint8_t *buf, uint32_t size,
                              uint16_t w, uint16_t h, uint8_t fmt);
void wifi_stream_camera_stopped(void);
void wifi_stream_notify_danger(uint32_t danger_count, float confidence);
int  wifi_stream_push_snapshot(const char *filename);

int       wifi_stream_set_http_enable(rt_bool_t en);
rt_bool_t wifi_stream_is_http_enabled(void);
int       wifi_stream_set_relay_config(const char *host, uint16_t port, rt_bool_t auto_push);

void wifi_stream_set_quality(int quality);
int  wifi_stream_get_quality(void);
void wifi_stream_set_max_fps(int fps);
int  wifi_stream_get_max_fps(void);

int  wifi_mgr_start_scan(void);
int  wifi_mgr_get_scan_results(wifi_ap_item_t *out, int max_items, rt_bool_t *scanning);
int  wifi_mgr_connect(const char *ssid, const char *pwd, rt_bool_t save_cfg);
int  wifi_mgr_disconnect(void);
void wifi_mgr_get_status(wifi_status_t *out);
rt_bool_t wifi_mgr_is_lan_peer(uint32_t peer_ip); /* IPv4 address in network order. */
uint32_t  wifi_mgr_get_station_ip(void);          /* IPv4 address in network order, or 0. */
uint32_t  wifi_mgr_get_lan_broadcast(void);       /* IPv4 subnet broadcast in network order, or 0. */

#ifdef __cplusplus
}
#endif

#endif /* WIFI_STREAM_H__ */
