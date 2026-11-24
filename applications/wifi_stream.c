/*
 * Wi-Fi Manager + Async JPEG Encoder + HTTP MJPEG Server (:8080) + TCP Relay Client (:8888)
 */

#include <rtthread.h>
#include <rtdevice.h>
#include <wlan_mgnt.h>
#include <wlan_cfg.h>
#include <arpa/inet.h>
#include <netdev.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <finsh.h>
#include <msh.h>

#include "wifi_stream.h"
#include "mini_jpeg_enc.h"
#include "uvc_ai_app.h"
#include "usbh_uvc_display_hook.h"
#include "alert_manager.h"

#define DBG_TAG "wifi.stream"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

#define WIFI_CFG_PATH           "/sdcard/wifi.cfg"
#define WIFI_RAW_MAX_BYTES      (320U * 240U * 2U)
#define WIFI_JPEG_MAX_BYTES     (64U * 1024U)
#define WIFI_HTTP_MAX_CLIENTS   2
#define WIFI_MGR_EVT_SCAN       0x01U
#define WIFI_MGR_EVT_CONNECT    0x02U
#define WIFI_MGR_EVT_DISCONNECT 0x04U

extern float g_aht_temp;
extern float g_aht_humi;

/* Weak hook overridden by dualcore_bridge.c in Step 4 */
__attribute__((weak)) void dualcore_get_brief(rt_bool_t *online, uint32_t *hb_seq, uint32_t *rtt_ms)
{
    if (online) *online = RT_FALSE;
    if (hb_seq) *hb_seq = 0U;
    if (rtt_ms) *rtt_ms = 0U;
}

typedef struct
{
    char     filename[48];
    uint32_t danger_count;
    float    confidence;
    rt_bool_t valid;
} relay_req_t;

static struct
{
    rt_bool_t init;
    rt_bool_t whd_ready;
    rt_bool_t connected;
    rt_bool_t ready;
    rt_bool_t scanning;
    rt_bool_t connecting;
    rt_bool_t http_enabled;
    rt_bool_t relay_auto_push;
    rt_bool_t last_relay_ok;

    int jpeg_quality;
    int max_fps;
    volatile int http_clients;

    char ssid[33];
    char pwd[65];
    char relay_host[64];
    char status_msg[48];
    uint16_t relay_port;
    rt_bool_t save_pending_cfg;

    /* Dynamic HyperRAM/internal heap buffers (no DTCM bloat) */
    uint8_t *raw_slot;
    uint16_t raw_w;
    uint16_t raw_h;
    uint8_t  raw_fmt;
    uint32_t raw_size;
    volatile rt_bool_t raw_pending;
    volatile rt_bool_t enc_busy;
    struct rt_mutex raw_lock;

    uint8_t *jpeg_buf[2];
    size_t   jpeg_len[2];
    volatile int active_jpeg_idx;
    volatile uint32_t jpeg_seq;
    struct rt_mutex jpeg_lock;

    uint8_t *relay_jpeg_copy;
    size_t   relay_jpeg_len;
    relay_req_t relay_req;
    struct rt_mutex relay_lock;

    volatile rt_bool_t danger_req;
    uint32_t danger_seq;
    float    danger_conf;
    volatile rt_bool_t snap_req;
    char     snap_name[48];

    uint32_t stream_fps;
    uint32_t tx_kbps;
    uint32_t frames_encoded;
    uint32_t relay_sent_count;
    uint32_t bytes_sent_accum;
    rt_tick_t stats_tick;

    rt_sem_t enc_sem;
    rt_sem_t relay_sem;
    rt_event_t mgr_evt;
    rt_sem_t scan_done_sem;

    wifi_ap_item_t scan_aps[WIFI_MGR_MAX_SCAN_APS];
    int scan_count;
    struct rt_mutex scan_lock;
} g_wf;

/* ----------------- Helpers ----------------- */

static void trim_crlf(char *s)
{
    size_t len;
    if (s == NULL) return;
    len = strlen(s);
    while (len > 0U && (s[len - 1U] == '\r' || s[len - 1U] == '\n' ||
                        s[len - 1U] == ' '  || s[len - 1U] == '\t'))
    {
        s[--len] = '\0';
    }
}

static void wifi_set_status_msg(const char *msg)
{
    if (msg == NULL) return;
    rt_strncpy(g_wf.status_msg, msg, sizeof(g_wf.status_msg) - 1U);
    g_wf.status_msg[sizeof(g_wf.status_msg) - 1U] = '\0';
}

static void wifi_save_cfg_file(const char *ssid, const char *pwd,
                               const char *relay_host, uint16_t relay_port)
{
    FILE *fp = fopen(WIFI_CFG_PATH, "w");
    if (fp == NULL)
    {
        LOG_W("cannot write %s", WIFI_CFG_PATH);
        return;
    }

    fprintf(fp, "ssid=%s\n", ssid ? ssid : "");
    fprintf(fp, "pwd=%s\n", pwd ? pwd : "");
    fprintf(fp, "relay_ip=%s\n", relay_host ? relay_host : "");
    fprintf(fp, "relay_port=%u\n", (unsigned)(relay_port ? relay_port : WIFI_STREAM_RELAY_PORT_DEF));
    fclose(fp);
    LOG_I("saved config to %s", WIFI_CFG_PATH);
}

static rt_bool_t wifi_load_cfg_file(char *ssid, size_t ssid_sz,
                                    char *pwd, size_t pwd_sz,
                                    char *relay_host, size_t rhost_sz,
                                    uint16_t *relay_port)
{
    FILE *fp;
    char line[128];
    rt_bool_t has_ssid = RT_FALSE;

    fp = fopen(WIFI_CFG_PATH, "r");
    if (fp == NULL)
    {
        return RT_FALSE;
    }

    while (fgets(line, sizeof(line), fp) != NULL)
    {
        char *eq;
        trim_crlf(line);
        if (line[0] == '#' || line[0] == '\0') continue;
        eq = strchr(line, '=');
        if (eq == NULL) continue;
        *eq = '\0';
        const char *k = line;
        const char *v = eq + 1;

        if (strcmp(k, "ssid") == 0 && ssid && ssid_sz > 0U)
        {
            rt_strncpy(ssid, v, ssid_sz - 1U);
            ssid[ssid_sz - 1U] = '\0';
            if (ssid[0] != '\0') has_ssid = RT_TRUE;
        }
        else if (strcmp(k, "pwd") == 0 && pwd && pwd_sz > 0U)
        {
            rt_strncpy(pwd, v, pwd_sz - 1U);
            pwd[pwd_sz - 1U] = '\0';
        }
        else if (strcmp(k, "relay_ip") == 0 && relay_host && rhost_sz > 0U)
        {
            rt_strncpy(relay_host, v, rhost_sz - 1U);
            relay_host[rhost_sz - 1U] = '\0';
        }
        else if (strcmp(k, "relay_port") == 0 && relay_port)
        {
            int p = atoi(v);
            if (p > 0 && p <= 65535) *relay_port = (uint16_t)p;
        }
    }

    fclose(fp);
    return has_ssid;
}

static struct netdev *wifi_station_netdev(void)
{
    struct rt_wlan_device *sta = (struct rt_wlan_device *)rt_device_find(RT_WLAN_DEVICE_STA_NAME);
    return sta != RT_NULL ? sta->netdev : RT_NULL;
}

static uint32_t wifi_station_ip(void)
{
    struct netdev *nd = wifi_station_netdev();
    return (g_wf.ready && nd != RT_NULL && netdev_is_up(nd)) ? ip4_addr_get_u32(&nd->ip_addr) : 0U;
}

rt_bool_t wifi_mgr_is_lan_peer(uint32_t peer_ip)
{
    struct netdev *nd = wifi_station_netdev();
    uint32_t ip = wifi_station_ip();
    uint32_t mask;

    if (nd == RT_NULL || ip == 0U)
        return RT_FALSE;

    mask = ip4_addr_get_u32(&nd->netmask);
    return mask != 0U && (peer_ip & mask) == (ip & mask) &&
           (peer_ip & ~mask) != 0U && (peer_ip & ~mask) != ~mask;
}

static void wifi_get_ip_string(char *buf, size_t len)
{
    struct netdev *nd = wifi_station_netdev();

    if ((nd != RT_NULL) && netdev_is_up(nd) && (ip4_addr_get_u32(&nd->ip_addr) != 0U))
    {
        const char *s = inet_ntoa(nd->ip_addr);
        if (s != NULL)
        {
            rt_strncpy(buf, s, len - 1U);
            buf[len - 1U] = '\0';
            return;
        }
    }
    rt_strncpy(buf, "0.0.0.0", len - 1U);
    buf[len - 1U] = '\0';
}

/* ----------------- WLAN Event Handlers ----------------- */

static void wf_wlan_event_cb(int event, struct rt_wlan_buff *buff, void *parameter)
{
    (void)buff;
    (void)parameter;

    switch (event)
    {
    case RT_WLAN_EVT_STA_CONNECTED:
        g_wf.connected = RT_TRUE;
        g_wf.connecting = RT_FALSE;
        wifi_set_status_msg("Connected (DHCP...)");
        break;
    case RT_WLAN_EVT_STA_CONNECTED_FAIL:
        g_wf.connected = RT_FALSE;
        g_wf.ready = RT_FALSE;
        g_wf.connecting = RT_FALSE;
        wifi_set_status_msg("Connect failed");
        break;
    case RT_WLAN_EVT_STA_DISCONNECTED:
        g_wf.connected = RT_FALSE;
        g_wf.ready = RT_FALSE;
        wifi_set_status_msg("Disconnected");
        break;
    case RT_WLAN_EVT_READY:
        g_wf.connected = RT_TRUE;
        g_wf.ready = RT_TRUE;
        g_wf.connecting = RT_FALSE;
        wifi_set_status_msg("Online");
        break;
    default:
        break;
    }
}

static void wf_scan_report_cb(int event, struct rt_wlan_buff *buff, void *parameter)
{
    struct rt_wlan_info *info;
    int i, weakest_idx = -1;

    (void)parameter;
    if ((event != RT_WLAN_EVT_SCAN_REPORT) || (buff == RT_NULL) || (buff->data == RT_NULL))
    {
        return;
    }

    info = (struct rt_wlan_info *)buff->data;
    if ((info->ssid.len == 0) || (info->ssid.len > 32))
    {
        return;
    }

    char ssid_str[33];
    rt_memcpy(ssid_str, info->ssid.val, info->ssid.len);
    ssid_str[info->ssid.len] = '\0';

    rt_mutex_take(&g_wf.scan_lock, RT_WAITING_FOREVER);

    /* Deduplicate by SSID, keep stronger RSSI */
    for (i = 0; i < g_wf.scan_count; i++)
    {
        if (strcmp(g_wf.scan_aps[i].ssid, ssid_str) == 0)
        {
            if (info->rssi > g_wf.scan_aps[i].rssi)
            {
                g_wf.scan_aps[i].rssi     = info->rssi;
                g_wf.scan_aps[i].channel  = (uint8_t)info->channel;
                g_wf.scan_aps[i].security = (uint32_t)info->security;
            }
            rt_mutex_release(&g_wf.scan_lock);
            return;
        }
    }

    if (g_wf.scan_count < WIFI_MGR_MAX_SCAN_APS)
    {
        wifi_ap_item_t *slot = &g_wf.scan_aps[g_wf.scan_count++];
        rt_strncpy(slot->ssid, ssid_str, sizeof(slot->ssid) - 1U);
        slot->ssid[sizeof(slot->ssid) - 1U] = '\0';
        slot->rssi     = info->rssi;
        slot->channel  = (uint8_t)info->channel;
        slot->security = (uint32_t)info->security;
    }
    else
    {
        int min_rssi = 0;
        for (i = 0; i < WIFI_MGR_MAX_SCAN_APS; i++)
        {
            if (weakest_idx < 0 || g_wf.scan_aps[i].rssi < min_rssi)
            {
                min_rssi = g_wf.scan_aps[i].rssi;
                weakest_idx = i;
            }
        }
        if ((weakest_idx >= 0) && (info->rssi > min_rssi))
        {
            wifi_ap_item_t *slot = &g_wf.scan_aps[weakest_idx];
            rt_strncpy(slot->ssid, ssid_str, sizeof(slot->ssid) - 1U);
            slot->ssid[sizeof(slot->ssid) - 1U] = '\0';
            slot->rssi     = info->rssi;
            slot->channel  = (uint8_t)info->channel;
            slot->security = (uint32_t)info->security;
        }
    }

    rt_mutex_release(&g_wf.scan_lock);
}

static void wf_scan_done_cb(int event, struct rt_wlan_buff *buff, void *parameter)
{
    (void)event;
    (void)buff;
    (void)parameter;
    if (g_wf.scan_done_sem != RT_NULL)
    {
        rt_sem_release(g_wf.scan_done_sem);
    }
}

static void wifi_sort_scan_results(void)
{
    int i, j;
    rt_mutex_take(&g_wf.scan_lock, RT_WAITING_FOREVER);
    for (i = 0; i < g_wf.scan_count - 1; i++)
    {
        for (j = i + 1; j < g_wf.scan_count; j++)
        {
            if (g_wf.scan_aps[j].rssi > g_wf.scan_aps[i].rssi)
            {
                wifi_ap_item_t tmp = g_wf.scan_aps[i];
                g_wf.scan_aps[i] = g_wf.scan_aps[j];
                g_wf.scan_aps[j] = tmp;
            }
        }
    }
    rt_mutex_release(&g_wf.scan_lock);
}

/* ----------------- JPEG Encoder & Relay Threads ----------------- */

static void wf_trigger_relay_push(const char *fname, uint32_t danger_cnt, float conf)
{
    if ((g_wf.relay_host[0] == '\0') || (g_wf.relay_jpeg_copy == NULL) ||
        (g_wf.relay_sem == RT_NULL))
    {
        return;
    }

    rt_mutex_take(&g_wf.jpeg_lock, RT_WAITING_FOREVER);
    int idx = g_wf.active_jpeg_idx;
    size_t len = g_wf.jpeg_len[idx];
    if (len == 0U || len > WIFI_JPEG_MAX_BYTES)
    {
        rt_mutex_release(&g_wf.jpeg_lock);
        return;
    }

    rt_mutex_take(&g_wf.relay_lock, RT_WAITING_FOREVER);
    rt_memcpy(g_wf.relay_jpeg_copy, g_wf.jpeg_buf[idx], len);
    g_wf.relay_jpeg_len = len;
    rt_strncpy(g_wf.relay_req.filename, fname, sizeof(g_wf.relay_req.filename) - 1U);
    g_wf.relay_req.filename[sizeof(g_wf.relay_req.filename) - 1U] = '\0';
    g_wf.relay_req.danger_count = danger_cnt;
    g_wf.relay_req.confidence   = conf;
    g_wf.relay_req.valid        = RT_TRUE;
    rt_mutex_release(&g_wf.relay_lock);

    rt_mutex_release(&g_wf.jpeg_lock);
    rt_sem_release(g_wf.relay_sem);
}

static size_t wf_encode_from_stage_fallback(uint8_t *dst, size_t cap)
{
    uint16_t w = 0, h = 0;
    const uint16_t *stage = uvc_display_get_stage_rgb565(&w, &h, RT_NULL);
    size_t out_sz = 0U;

    if (stage == NULL || w == 0 || h == 0) return 0U;
    uvc_display_lock_stage();
    out_sz = mini_jpeg_encode_rgb565(stage, (int)w, (int)h, g_wf.jpeg_quality, dst, cap);
    uvc_display_unlock_stage();
    return out_sz;
}

static void wf_enc_thread_entry(void *parameter)
{
    rt_tick_t last_fps_tick = rt_tick_get();
    uint32_t fps_cnt = 0U;
    (void)parameter;

    while (1)
    {
        rt_sem_take(g_wf.enc_sem, RT_WAITING_FOREVER);

        rt_bool_t do_danger = g_wf.danger_req;
        rt_bool_t do_snap   = g_wf.snap_req;
        rt_bool_t do_frame  = g_wf.raw_pending;

        if (!do_danger && !do_snap && !do_frame)
        {
            continue;
        }

        g_wf.enc_busy = RT_TRUE;

        /* Encode next back buffer */
        int back_idx = 1 - g_wf.active_jpeg_idx;
        uint8_t *dst = g_wf.jpeg_buf[back_idx];
        size_t enc_len = 0U;

        if (dst != NULL)
        {
            if (g_wf.raw_pending && g_wf.raw_slot != NULL && g_wf.raw_size > 0U)
            {
                rt_mutex_take(&g_wf.raw_lock, RT_WAITING_FOREVER);
                uint16_t w = g_wf.raw_w;
                uint16_t h = g_wf.raw_h;
                uint8_t fmt = g_wf.raw_fmt;
                uint32_t sz = g_wf.raw_size;

                if (fmt == 0U /* YUYV */ && sz >= (uint32_t)w * (uint32_t)h * 2U)
                {
                    enc_len = mini_jpeg_encode_yuyv(g_wf.raw_slot, (int)w, (int)h,
                                                    g_wf.jpeg_quality, dst, WIFI_JPEG_MAX_BYTES);
                }
                else if (fmt == 1U /* MJPEG */ && sz > 4U && sz <= WIFI_JPEG_MAX_BYTES &&
                         g_wf.raw_slot[0] == 0xFFU && g_wf.raw_slot[1] == 0xD8U)
                {
                    rt_memcpy(dst, g_wf.raw_slot, sz);
                    enc_len = sz;
                }
                g_wf.raw_pending = RT_FALSE;
                rt_mutex_release(&g_wf.raw_lock);
            }
            else if (do_danger || do_snap)
            {
                enc_len = wf_encode_from_stage_fallback(dst, WIFI_JPEG_MAX_BYTES);
            }

            if (enc_len > 0U)
            {
                rt_mutex_take(&g_wf.jpeg_lock, RT_WAITING_FOREVER);
                g_wf.jpeg_len[back_idx] = enc_len;
                g_wf.active_jpeg_idx = back_idx;
                g_wf.jpeg_seq++;
                g_wf.frames_encoded++;
                rt_mutex_release(&g_wf.jpeg_lock);

                fps_cnt++;
                rt_tick_t now = rt_tick_get();
                uint32_t dt_ms = ((uint32_t)(now - last_fps_tick)) * (1000U / RT_TICK_PER_SECOND);
                if (dt_ms >= 1000U)
                {
                    g_wf.stream_fps = (fps_cnt * 1000U) / dt_ms;
                    fps_cnt = 0U;
                    last_fps_tick = now;
                }
            }
        }

        if (do_danger)
        {
            g_wf.danger_req = RT_FALSE;
            if (g_wf.relay_auto_push)
            {
                char fname[48];
                rt_snprintf(fname, sizeof(fname), "danger_%04lu.jpg", (unsigned long)g_wf.danger_seq);
                wf_trigger_relay_push(fname, g_wf.danger_seq, g_wf.danger_conf);
            }
        }

        if (do_snap)
        {
            char fname[48];
            rt_strncpy(fname, g_wf.snap_name, sizeof(fname) - 1U);
            fname[sizeof(fname) - 1U] = '\0';
            g_wf.snap_req = RT_FALSE;
            if (fname[0] == '\0')
            {
                rt_snprintf(fname, sizeof(fname), "snap_%04lu.jpg", (unsigned long)g_wf.frames_encoded);
            }
            wf_trigger_relay_push(fname, 0U, 0.0f);
        }

        g_wf.enc_busy = RT_FALSE;
    }
}

static int tcp_send_all(int fd, const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    size_t sent = 0U;

    while (sent < len)
    {
        int n = send(fd, p + sent, len - sent, 0);
        if (n <= 0)
        {
            return -1;
        }
        sent += (size_t)n;
    }
    return 0;
}

static void wf_relay_thread_entry(void *parameter)
{
    (void)parameter;

    while (1)
    {
        rt_sem_take(g_wf.relay_sem, RT_WAITING_FOREVER);

        if (!rt_wlan_is_ready() || (g_wf.relay_host[0] == '\0'))
        {
            g_wf.last_relay_ok = RT_FALSE;
            continue;
        }

        rt_mutex_take(&g_wf.relay_lock, RT_WAITING_FOREVER);
        if (!g_wf.relay_req.valid || g_wf.relay_jpeg_len == 0U)
        {
            rt_mutex_release(&g_wf.relay_lock);
            continue;
        }

        char fname[48];
        rt_strncpy(fname, g_wf.relay_req.filename, sizeof(fname) - 1U);
        fname[sizeof(fname) - 1U] = '\0';
        size_t jpeg_sz = g_wf.relay_jpeg_len;
        uint8_t *payload = (uint8_t *)rt_malloc(jpeg_sz);
        if (payload != NULL)
        {
            rt_memcpy(payload, g_wf.relay_jpeg_copy, jpeg_sz);
        }
        g_wf.relay_req.valid = RT_FALSE;
        rt_mutex_release(&g_wf.relay_lock);

        if (payload == NULL)
        {
            g_wf.last_relay_ok = RT_FALSE;
            continue;
        }

        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0)
        {
            rt_free(payload);
            g_wf.last_relay_ok = RT_FALSE;
            continue;
        }

        struct timeval tv = { .tv_sec = 3, .tv_usec = 0 };
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        struct sockaddr_in addr;
        rt_memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(g_wf.relay_port ? g_wf.relay_port : WIFI_STREAM_RELAY_PORT_DEF);
        addr.sin_addr.s_addr = inet_addr(g_wf.relay_host);

        if (addr.sin_addr.s_addr == IPADDR_NONE)
        {
            struct hostent *he = gethostbyname(g_wf.relay_host);
            if (he != NULL && he->h_addr_list[0] != NULL)
            {
                rt_memcpy(&addr.sin_addr, he->h_addr_list[0], sizeof(addr.sin_addr));
            }
        }

        if (wifi_mgr_is_lan_peer(addr.sin_addr.s_addr) &&
            connect(sock, (struct sockaddr *)&addr, sizeof(addr)) == 0)
        {
            uint32_t name_len = (uint32_t)strlen(fname) + 1U;
            uint32_t be_nlen  = htonl(name_len);
            uint32_t be_flen  = htonl((uint32_t)jpeg_sz);

            if ((tcp_send_all(sock, &be_nlen, 4) == 0) &&
                (tcp_send_all(sock, fname, name_len) == 0) &&
                (tcp_send_all(sock, &be_flen, 4) == 0) &&
                (tcp_send_all(sock, payload, jpeg_sz) == 0))
            {
                g_wf.relay_sent_count++;
                g_wf.last_relay_ok = RT_TRUE;
                LOG_I("relay sent %s (%u B) -> %s:%u",
                      fname, (unsigned)jpeg_sz, g_wf.relay_host, (unsigned)g_wf.relay_port);
            }
            else
            {
                g_wf.last_relay_ok = RT_FALSE;
                LOG_W("relay send failed to %s:%u", g_wf.relay_host, (unsigned)g_wf.relay_port);
            }
        }
        else
        {
            g_wf.last_relay_ok = RT_FALSE;
            LOG_W("relay connect failed %s:%u", g_wf.relay_host, (unsigned)g_wf.relay_port);
        }

        closesocket(sock);
        rt_free(payload);
    }
}

/* ----------------- HTTP MJPEG Server (:8080) ----------------- */

static const char g_html_index[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/html; charset=utf-8\r\n"
    "Connection: close\r\n\r\n"
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Edgi-Talk Driver Safety Cockpit</title>"
    "<style>"
    "body{margin:0;background:#0B0F1A;color:#F1F5F9;font-family:system-ui,sans-serif;display:flex;flex-direction:column;align-items:center;padding:16px}"
    ".card{background:#141C2E;border:1px solid #23304A;border-radius:14px;padding:14px;max-width:680px;width:100%;margin-bottom:12px}"
    "img{width:100%;border-radius:10px;background:#060911;border:1px solid #23304A}"
    ".grid{display:grid;grid-template-columns:repeat(3,1fr);gap:10px;margin-top:10px}"
    ".kpi{background:#0F1626;border-radius:10px;padding:10px;text-align:center}"
    ".v{font-size:20px;font-weight:700;color:#38BDF8}.l{font-size:12px;color:#94A3B8}"
    "</style></head><body>"
    "<div class='card'><h2 style='margin:0 0 10px'>Driver Fatigue Monitor — Live Wi-Fi Feed</h2>"
    "<img src='/stream' alt='MJPEG Stream'/>"
    "<div class='grid'>"
    "<div class='kpi'><div class='v' id='risk'>--</div><div class='l'>Fatigue Risk</div></div>"
    "<div class='kpi'><div class='v' id='temp'>--</div><div class='l'>Cabin T / H</div></div>"
    "<div class='kpi'><div class='v' id='fps'>--</div><div class='l'>Stream / Core</div></div>"
    "</div></div>"
    "<script>setInterval(async()=>{try{const r=await fetch('/api/status');const j=await r.json();"
    "document.getElementById('risk').innerText=j.risk_label+' ('+j.conf_pct+'%)';"
    "document.getElementById('risk').style.color=j.alert_active?'#EF4444':(j.risk_level>=1?'#F59E0B':'#10B981');"
    "document.getElementById('temp').innerText=j.temp+'C / '+j.humi+'%';"
    "document.getElementById('fps').innerText=j.stream_fps+' FPS / '+(j.dualcore?'DUAL':'SOLO');"
    "}catch(e){}},700);</script></body></html>";

static void wf_mjpeg_client_entry(void *parameter)
{
    int client_fd = (int)(intptr_t)parameter;
    uint8_t *local_buf = (uint8_t *)rt_malloc(WIFI_JPEG_MAX_BYTES);
    uint32_t last_seq = 0U;
    char hdr[96];

    if (local_buf == NULL)
    {
        closesocket(client_fd);
        g_wf.http_clients--;
        return;
    }

    static const char mjpeg_hdr[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n"
        "Cache-Control: no-cache\r\n"
        "Pragma: no-cache\r\n"
        "Connection: close\r\n\r\n";

    if (tcp_send_all(client_fd, mjpeg_hdr, sizeof(mjpeg_hdr) - 1U) != 0)
    {
        rt_free(local_buf);
        closesocket(client_fd);
        g_wf.http_clients--;
        return;
    }

    while (g_wf.http_enabled && g_wf.ready)
    {
        uint32_t cur_seq = g_wf.jpeg_seq;
        if (cur_seq != last_seq)
        {
            rt_mutex_take(&g_wf.jpeg_lock, RT_WAITING_FOREVER);
            int idx = g_wf.active_jpeg_idx;
            size_t len = g_wf.jpeg_len[idx];
            if (len > 0U && len <= WIFI_JPEG_MAX_BYTES)
            {
                rt_memcpy(local_buf, g_wf.jpeg_buf[idx], len);
                last_seq = g_wf.jpeg_seq;
            }
            else
            {
                len = 0U;
            }
            rt_mutex_release(&g_wf.jpeg_lock);

            if (len > 0U)
            {
                int hlen = rt_snprintf(hdr, sizeof(hdr),
                                       "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
                                       (unsigned)len);
                if ((tcp_send_all(client_fd, hdr, (size_t)hlen) != 0) ||
                    (tcp_send_all(client_fd, local_buf, len) != 0) ||
                    (tcp_send_all(client_fd, "\r\n", 2U) != 0))
                {
                    break;
                }
                g_wf.bytes_sent_accum += (uint32_t)(hlen + len + 2);
                rt_tick_t now = rt_tick_get();
                uint32_t dt = ((uint32_t)(now - g_wf.stats_tick)) * (1000U / RT_TICK_PER_SECOND);
                if (dt >= 1000U)
                {
                    g_wf.tx_kbps = (g_wf.bytes_sent_accum * 8U) / dt;
                    g_wf.bytes_sent_accum = 0U;
                    g_wf.stats_tick = now;
                }
            }
        }
        int delay_ms = (g_wf.max_fps > 0) ? (1000 / g_wf.max_fps) : 120;
        if (delay_ms < 40) delay_ms = 40;
        rt_thread_mdelay((rt_int32_t)delay_ms);
    }

    rt_free(local_buf);
    closesocket(client_fd);
    if (g_wf.http_clients > 0) g_wf.http_clients--;
}

static void wf_handle_single_http_req(int cfd)
{
    char req[256];
    int n;
    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };

    setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    tv.tv_sec = 3;
    setsockopt(cfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    int nodelay = 1;
    setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    n = recv(cfd, req, sizeof(req) - 1, 0);
    if (n <= 0)
    {
        closesocket(cfd);
        return;
    }
    req[n] = '\0';

    if (strncmp(req, "GET /stream", 11) == 0)
    {
        if (g_wf.http_clients < WIFI_HTTP_MAX_CLIENTS)
        {
            rt_thread_t t = rt_thread_create("wf_mjpg", wf_mjpeg_client_entry,
                                             (void *)(intptr_t)cfd, 2560, 17, 10);
            if (t != RT_NULL)
            {
                g_wf.http_clients++;
                rt_thread_startup(t);
                return;
            }
        }
        static const char busy[] = "HTTP/1.1 503 Busy\r\nConnection: close\r\n\r\nMax stream clients reached\n";
        (void)tcp_send_all(cfd, busy, sizeof(busy) - 1U);
        closesocket(cfd);
        return;
    }

    if (strncmp(req, "GET /snapshot.jpg", 17) == 0)
    {
        uint8_t *snap = (uint8_t *)rt_malloc(WIFI_JPEG_MAX_BYTES);
        size_t len = 0U;
        if (snap != NULL)
        {
            rt_mutex_take(&g_wf.jpeg_lock, RT_WAITING_FOREVER);
            int idx = g_wf.active_jpeg_idx;
            len = g_wf.jpeg_len[idx];
            if (len > 0U && len <= WIFI_JPEG_MAX_BYTES)
            {
                rt_memcpy(snap, g_wf.jpeg_buf[idx], len);
            }
            else
            {
                len = 0U;
            }
            rt_mutex_release(&g_wf.jpeg_lock);

            if (len == 0U)
            {
                len = wf_encode_from_stage_fallback(snap, WIFI_JPEG_MAX_BYTES);
            }

            if (len > 0U)
            {
                char hdr[96];
                int hlen = rt_snprintf(hdr, sizeof(hdr),
                                       "HTTP/1.1 200 OK\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\nConnection: close\r\n\r\n",
                                       (unsigned)len);
                (void)tcp_send_all(cfd, hdr, (size_t)hlen);
                (void)tcp_send_all(cfd, snap, len);
            }
            else
            {
                static const char empty[] = "HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\nNo frame yet\n";
                (void)tcp_send_all(cfd, empty, sizeof(empty) - 1U);
            }
            rt_free(snap);
        }
        closesocket(cfd);
        return;
    }

    if (strncmp(req, "GET /api/status", 15) == 0)
    {
        uvc_ai_telemetry_t tel;
        uint32_t cam_fps = 0, disp_fps = 0, drops = 0;
        rt_bool_t dc_on = RT_FALSE;
        uint32_t m33_hb = 0, m33_rtt = 0;
        char ip[20];
        char body[384];
        char resp[512];
        static const char *risk_names[3] = { "SAFE", "MID", "HIGH" };

        uvc_ai_app_get_telemetry(&tel);
        uvc_app_get_fps(&cam_fps, &disp_fps, &drops);
        dualcore_get_brief(&dc_on, &m33_hb, &m33_rtt);
        wifi_get_ip_string(ip, sizeof(ip));
        uint8_t ridx = (tel.risk_level < 3U) ? tel.risk_level : 0U;

        int blen = rt_snprintf(
            body, sizeof(body),
            "{\"running\":%s,\"alert_active\":%s,\"risk_level\":%u,\"risk_label\":\"%s\","
            "\"conf_pct\":%.0f,\"inference_ms\":%.1f,\"danger_count\":%lu,"
            "\"temp\":%.1f,\"humi\":%.1f,\"cam_fps\":%lu,\"stream_fps\":%lu,"
            "\"tx_kbps\":%lu,\"rssi\":%d,\"ip\":\"%s\",\"dualcore\":%s,"
            "\"m33_hb\":%lu,\"m33_rtt_ms\":%lu}",
            tel.running ? "true" : "false",
            tel.alert_active ? "true" : "false",
            (unsigned)ridx, risk_names[ridx],
            (double)(tel.max_closed_conf * 100.0f),
            (double)tel.inference_ms,
            (unsigned long)tel.danger_count,
            (double)g_aht_temp, (double)g_aht_humi,
            (unsigned long)cam_fps, (unsigned long)g_wf.stream_fps,
            (unsigned long)g_wf.tx_kbps, rt_wlan_is_connected() ? rt_wlan_get_rssi() : 0,
            ip, dc_on ? "true" : "false",
            (unsigned long)m33_hb, (unsigned long)m33_rtt);

        int hlen = rt_snprintf(resp, sizeof(resp),
                               "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %d\r\nConnection: close\r\n\r\n%s",
                               blen, body);
        (void)tcp_send_all(cfd, resp, (size_t)hlen);
        closesocket(cfd);
        return;
    }

    (void)tcp_send_all(cfd, g_html_index, sizeof(g_html_index) - 1U);
    closesocket(cfd);
}

static void wf_http_thread_entry(void *parameter)
{
    int listen_fd = -1;
    uint32_t bound_ip = 0U;
    (void)parameter;

    while (!g_wf.whd_ready)
    {
        rt_thread_mdelay(250);
    }

    while (1)
    {
        uint32_t station_ip = wifi_station_ip();
        if (listen_fd >= 0 && (!g_wf.http_enabled || station_ip != bound_ip))
        {
            closesocket(listen_fd);
            listen_fd = -1;
        }
        if (!g_wf.http_enabled || station_ip == 0U)
        {
            rt_thread_mdelay(200);
            continue;
        }

        if (listen_fd < 0)
        {
            int opt = 1;
            struct sockaddr_in saddr;
            listen_fd = socket(AF_INET, SOCK_STREAM, 0);
            if (listen_fd < 0)
            {
                rt_thread_mdelay(500);
                continue;
            }
            setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
            rt_memset(&saddr, 0, sizeof(saddr));
            saddr.sin_family = AF_INET;
            saddr.sin_port = htons(WIFI_STREAM_HTTP_PORT);
            saddr.sin_addr.s_addr = station_ip;

            if ((bind(listen_fd, (struct sockaddr *)&saddr, sizeof(saddr)) != 0) ||
                (listen(listen_fd, 4) != 0))
            {
                closesocket(listen_fd);
                listen_fd = -1;
                rt_thread_mdelay(1000);
                continue;
            }
            bound_ip = station_ip;
            LOG_I("LAN MJPEG server listening on :%d", WIFI_STREAM_HTTP_PORT);
        }

        fd_set rfds;
        struct timeval tv = { .tv_sec = 0, .tv_usec = 400000 };
        FD_ZERO(&rfds);
        FD_SET(listen_fd, &rfds);

        if (select(listen_fd + 1, &rfds, NULL, NULL, &tv) > 0 && FD_ISSET(listen_fd, &rfds))
        {
            struct sockaddr_in caddr;
            socklen_t clen = sizeof(caddr);
            int cfd = accept(listen_fd, (struct sockaddr *)&caddr, &clen);
            if (cfd >= 0)
            {
                if (wifi_mgr_is_lan_peer(caddr.sin_addr.s_addr))
                    wf_handle_single_http_req(cfd);
                else
                    closesocket(cfd);
            }
        }
    }
}

/* ----------------- Wi-Fi Manager Thread ----------------- */

static void wf_mgr_thread_entry(void *parameter)
{
    int i;
    (void)parameter;

    /* Wait asynchronously for WHD firmware download to register wlan0 */
    for (i = 0; i < 80; i++)
    {
        if (rt_device_find(RT_WLAN_DEVICE_STA_NAME) != RT_NULL)
        {
            g_wf.whd_ready = RT_TRUE;
            break;
        }
        rt_thread_mdelay(250);
    }

    if (!g_wf.whd_ready)
    {
        wifi_set_status_msg("Wi-Fi HW unavailable");
        LOG_W("wlan0 did not register within 20s");
    }
    else
    {
        wifi_set_status_msg("Ready (Disconnected)");
        rt_wlan_register_event_handler(RT_WLAN_EVT_STA_CONNECTED, wf_wlan_event_cb, RT_NULL);
        rt_wlan_register_event_handler(RT_WLAN_EVT_STA_CONNECTED_FAIL, wf_wlan_event_cb, RT_NULL);
        rt_wlan_register_event_handler(RT_WLAN_EVT_STA_DISCONNECTED, wf_wlan_event_cb, RT_NULL);
        rt_wlan_register_event_handler(RT_WLAN_EVT_READY, wf_wlan_event_cb, RT_NULL);
        LOG_I("wlan0 ready");
    }

    /* Wait briefly for /sdcard mount, then load saved credentials */
    for (i = 0; i < 20; i++)
    {
        if (access("/sdcard", 0) == 0)
        {
            break;
        }
        rt_thread_mdelay(150);
    }

    {
        char cfg_ssid[33] = {0};
        char cfg_pwd[65]  = {0};
        if (wifi_load_cfg_file(cfg_ssid, sizeof(cfg_ssid),
                               cfg_pwd, sizeof(cfg_pwd),
                               g_wf.relay_host, sizeof(g_wf.relay_host),
                               &g_wf.relay_port) && g_wf.whd_ready)
        {
            rt_strncpy(g_wf.ssid, cfg_ssid, sizeof(g_wf.ssid) - 1U);
            rt_strncpy(g_wf.pwd,  cfg_pwd,  sizeof(g_wf.pwd) - 1U);
            g_wf.connecting = RT_TRUE;
            wifi_set_status_msg("Auto-connecting...");
            rt_wlan_connect(g_wf.ssid, g_wf.pwd);
        }
    }

    while (1)
    {
        rt_uint32_t recved = 0U;
        if (rt_event_recv(g_wf.mgr_evt,
                          WIFI_MGR_EVT_SCAN | WIFI_MGR_EVT_CONNECT | WIFI_MGR_EVT_DISCONNECT,
                          RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR,
                          RT_WAITING_FOREVER, &recved) != RT_EOK)
        {
            continue;
        }

        if (!g_wf.whd_ready)
        {
            g_wf.scanning = RT_FALSE;
            g_wf.connecting = RT_FALSE;
            continue;
        }

        if (recved & WIFI_MGR_EVT_SCAN)
        {
            rt_mutex_take(&g_wf.scan_lock, RT_WAITING_FOREVER);
            g_wf.scan_count = 0;
            rt_mutex_release(&g_wf.scan_lock);

            rt_wlan_register_event_handler(RT_WLAN_EVT_SCAN_REPORT, wf_scan_report_cb, RT_NULL);
            rt_wlan_register_event_handler(RT_WLAN_EVT_SCAN_DONE,   wf_scan_done_cb,   RT_NULL);

            if (rt_wlan_scan_with_info(RT_NULL) != RT_EOK)
            {
                if (rt_wlan_scan() == RT_EOK && g_wf.scan_done_sem != RT_NULL)
                {
                    rt_sem_take(g_wf.scan_done_sem, rt_tick_from_millisecond(8000));
                }
            }

            rt_wlan_unregister_event_handler(RT_WLAN_EVT_SCAN_REPORT);
            rt_wlan_unregister_event_handler(RT_WLAN_EVT_SCAN_DONE);
            wifi_sort_scan_results();
            g_wf.scanning = RT_FALSE;
        }

        if (recved & WIFI_MGR_EVT_CONNECT)
        {
            wifi_set_status_msg("Connecting...");
            if (rt_wlan_connect(g_wf.ssid, g_wf.pwd) == RT_EOK)
            {
                if (g_wf.save_pending_cfg)
                {
                    wifi_save_cfg_file(g_wf.ssid, g_wf.pwd, g_wf.relay_host, g_wf.relay_port);
                }
            }
            else
            {
                g_wf.connecting = RT_FALSE;
                wifi_set_status_msg("Connect failed");
            }
            g_wf.save_pending_cfg = RT_FALSE;
        }

        if (recved & WIFI_MGR_EVT_DISCONNECT)
        {
            rt_wlan_disconnect();
            g_wf.connected = RT_FALSE;
            g_wf.ready = RT_FALSE;
            wifi_set_status_msg("Disconnected");
        }
    }
}

/* ----------------- Public APIs ----------------- */

int wifi_stream_init(void)
{
    if (g_wf.init)
    {
        return RT_EOK;
    }

    rt_memset(&g_wf, 0, sizeof(g_wf));
    g_wf.http_enabled    = RT_TRUE;
    g_wf.relay_auto_push = RT_TRUE;
    g_wf.jpeg_quality    = 65;
    g_wf.max_fps         = 8;
    g_wf.relay_port      = WIFI_STREAM_RELAY_PORT_DEF;
    g_wf.stats_tick      = rt_tick_get();
    wifi_set_status_msg("Initializing Wi-Fi...");

    rt_mutex_init(&g_wf.raw_lock,   "wf_raw", RT_IPC_FLAG_PRIO);
    rt_mutex_init(&g_wf.jpeg_lock,  "wf_jpg", RT_IPC_FLAG_PRIO);
    rt_mutex_init(&g_wf.relay_lock, "wf_rly", RT_IPC_FLAG_PRIO);
    rt_mutex_init(&g_wf.scan_lock,  "wf_scn", RT_IPC_FLAG_PRIO);

    g_wf.raw_slot        = (uint8_t *)rt_malloc(WIFI_RAW_MAX_BYTES);
    g_wf.jpeg_buf[0]     = (uint8_t *)rt_malloc(WIFI_JPEG_MAX_BYTES);
    g_wf.jpeg_buf[1]     = (uint8_t *)rt_malloc(WIFI_JPEG_MAX_BYTES);
    g_wf.relay_jpeg_copy = (uint8_t *)rt_malloc(WIFI_JPEG_MAX_BYTES);

    g_wf.enc_sem       = rt_sem_create("wf_enc", 0, RT_IPC_FLAG_FIFO);
    g_wf.relay_sem     = rt_sem_create("wf_rly", 0, RT_IPC_FLAG_FIFO);
    g_wf.scan_done_sem = rt_sem_create("wf_scn", 0, RT_IPC_FLAG_FIFO);
    g_wf.mgr_evt       = rt_event_create("wf_mgr", RT_IPC_FLAG_FIFO);

    rt_thread_t t;
    t = rt_thread_create("wf_enc", wf_enc_thread_entry, RT_NULL, 6144, 18, 10);
    if (t) rt_thread_startup(t);

    t = rt_thread_create("wf_rly", wf_relay_thread_entry, RT_NULL, 4096, 19, 10);
    if (t) rt_thread_startup(t);

    t = rt_thread_create("wf_http", wf_http_thread_entry, RT_NULL, 4096, 16, 10);
    if (t) rt_thread_startup(t);

    t = rt_thread_create("wf_mgr", wf_mgr_thread_entry, RT_NULL, 3072, 16, 10);
    if (t) rt_thread_startup(t);

    g_wf.init = RT_TRUE;
    LOG_I("wifi_stream ready (http=:%d, relay_def=:%d)",
          WIFI_STREAM_HTTP_PORT, WIFI_STREAM_RELAY_PORT_DEF);
    return RT_EOK;
}

void wifi_stream_submit_frame(const uint8_t *buf, uint32_t size,
                              uint16_t w, uint16_t h, uint8_t fmt)
{
    static rt_tick_t last_submit_tick = 0U;
    rt_tick_t now;
    uint32_t min_interval_ms;

    if (!g_wf.init || (buf == NULL) || (size == 0U) || (g_wf.raw_slot == NULL))
    {
        return;
    }

    /* Skip copy/encode when idle unless HTTP stream or snapshot/danger needs fresh frame */
    rt_bool_t urgent = g_wf.danger_req || g_wf.snap_req;
    rt_bool_t active_stream = g_wf.http_enabled && (g_wf.http_clients > 0);
    if (!urgent && !active_stream && (g_wf.jpeg_len[g_wf.active_jpeg_idx] > 0U))
    {
        return;
    }

    now = rt_tick_get();
    min_interval_ms = (g_wf.max_fps > 0) ? (1000U / (uint32_t)g_wf.max_fps) : 125U;
    if (!urgent && last_submit_tick != 0U)
    {
        uint32_t dt = ((uint32_t)(now - last_submit_tick)) * (1000U / RT_TICK_PER_SECOND);
        if (dt < min_interval_ms)
        {
            return;
        }
    }

    if (g_wf.enc_busy || g_wf.raw_pending || (size > WIFI_RAW_MAX_BYTES))
    {
        return;
    }

    if (rt_mutex_take(&g_wf.raw_lock, 0) != RT_EOK)
    {
        return;
    }

    rt_memcpy(g_wf.raw_slot, buf, size);
    g_wf.raw_size = size;
    g_wf.raw_w    = w;
    g_wf.raw_h    = h;
    g_wf.raw_fmt  = fmt;
    g_wf.raw_pending = RT_TRUE;
    last_submit_tick = now;
    rt_mutex_release(&g_wf.raw_lock);

    if (g_wf.enc_sem != RT_NULL)
    {
        rt_sem_release(g_wf.enc_sem);
    }
}

void wifi_stream_notify_danger(uint32_t danger_count, float confidence)
{
    if (!g_wf.init)
    {
        return;
    }

    g_wf.danger_seq  = danger_count;
    g_wf.danger_conf = confidence;
    g_wf.danger_req  = RT_TRUE;
    if (g_wf.enc_sem != RT_NULL)
    {
        rt_sem_release(g_wf.enc_sem);
    }
}

int wifi_stream_push_snapshot(const char *filename)
{
    if (!g_wf.init)
    {
        return -RT_ERROR;
    }

    if (filename != NULL && filename[0] != '\0')
    {
        rt_strncpy(g_wf.snap_name, filename, sizeof(g_wf.snap_name) - 1U);
        g_wf.snap_name[sizeof(g_wf.snap_name) - 1U] = '\0';
    }
    else
    {
        g_wf.snap_name[0] = '\0';
    }

    g_wf.snap_req = RT_TRUE;
    if (g_wf.enc_sem != RT_NULL)
    {
        rt_sem_release(g_wf.enc_sem);
    }
    return RT_EOK;
}

int wifi_stream_set_http_enable(rt_bool_t en)
{
    g_wf.http_enabled = en;
    return RT_EOK;
}

rt_bool_t wifi_stream_is_http_enabled(void)
{
    return g_wf.http_enabled;
}

int wifi_stream_set_relay_config(const char *host, uint16_t port, rt_bool_t auto_push)
{
    if (host != NULL)
    {
        rt_strncpy(g_wf.relay_host, host, sizeof(g_wf.relay_host) - 1U);
        g_wf.relay_host[sizeof(g_wf.relay_host) - 1U] = '\0';
    }
    if (port > 0U)
    {
        g_wf.relay_port = port;
    }
    g_wf.relay_auto_push = auto_push;
    return RT_EOK;
}

void wifi_stream_set_quality(int quality)
{
    if (quality < 30) quality = 30;
    if (quality > 90) quality = 90;
    g_wf.jpeg_quality = quality;
}

int wifi_stream_get_quality(void)
{
    return g_wf.jpeg_quality;
}

void wifi_stream_set_max_fps(int fps)
{
    if (fps < 1) fps = 1;
    if (fps > 20) fps = 20;
    g_wf.max_fps = fps;
}

int wifi_stream_get_max_fps(void)
{
    return g_wf.max_fps;
}

int wifi_mgr_start_scan(void)
{
    if (!g_wf.init || g_wf.mgr_evt == RT_NULL) return -RT_ERROR;
    if (!g_wf.whd_ready) return -RT_EBUSY;
    if (g_wf.scanning) return RT_EOK;

    g_wf.scanning = RT_TRUE;
    return rt_event_send(g_wf.mgr_evt, WIFI_MGR_EVT_SCAN);
}

int wifi_mgr_get_scan_results(wifi_ap_item_t *out, int max_items, rt_bool_t *scanning)
{
    int n = 0;
    if (scanning != NULL) *scanning = g_wf.scanning;
    if ((out == NULL) || (max_items <= 0)) return 0;

    rt_mutex_take(&g_wf.scan_lock, RT_WAITING_FOREVER);
    n = (g_wf.scan_count < max_items) ? g_wf.scan_count : max_items;
    if (n > 0)
    {
        rt_memcpy(out, g_wf.scan_aps, (size_t)n * sizeof(wifi_ap_item_t));
    }
    rt_mutex_release(&g_wf.scan_lock);
    return n;
}

int wifi_mgr_connect(const char *ssid, const char *pwd, rt_bool_t save_cfg)
{
    if (!g_wf.init || ssid == NULL || ssid[0] == '\0') return -RT_EINVAL;
    if (!g_wf.whd_ready) return -RT_EBUSY;

    rt_strncpy(g_wf.ssid, ssid, sizeof(g_wf.ssid) - 1U);
    g_wf.ssid[sizeof(g_wf.ssid) - 1U] = '\0';
    if (pwd != NULL)
    {
        rt_strncpy(g_wf.pwd, pwd, sizeof(g_wf.pwd) - 1U);
        g_wf.pwd[sizeof(g_wf.pwd) - 1U] = '\0';
    }
    else
    {
        g_wf.pwd[0] = '\0';
    }
    g_wf.save_pending_cfg = save_cfg;
    g_wf.connecting = RT_TRUE;
    return rt_event_send(g_wf.mgr_evt, WIFI_MGR_EVT_CONNECT);
}

int wifi_mgr_disconnect(void)
{
    if (!g_wf.init || g_wf.mgr_evt == RT_NULL) return -RT_ERROR;
    return rt_event_send(g_wf.mgr_evt, WIFI_MGR_EVT_DISCONNECT);
}

void wifi_mgr_get_status(wifi_status_t *out)
{
    struct rt_wlan_info info;
    if (out == NULL) return;

    rt_memset(out, 0, sizeof(*out));
    out->whd_ready       = g_wf.whd_ready;
    out->connected       = rt_wlan_is_connected();
    out->ready           = rt_wlan_is_ready();
    out->scanning        = g_wf.scanning;
    out->connecting      = g_wf.connecting;
    out->http_enabled    = g_wf.http_enabled;
    out->relay_auto_push = g_wf.relay_auto_push;
    out->last_relay_ok   = g_wf.last_relay_ok;
    out->http_clients    = g_wf.http_clients;
    out->jpeg_quality    = g_wf.jpeg_quality;
    out->max_fps         = g_wf.max_fps;
    out->relay_port      = g_wf.relay_port;
    out->stream_fps      = (g_wf.http_clients > 0) ? g_wf.stream_fps : 0U;
    out->tx_kbps         = (g_wf.http_clients > 0) ? g_wf.tx_kbps : 0U;
    out->frames_encoded  = g_wf.frames_encoded;
    out->relay_sent_count = g_wf.relay_sent_count;
    out->rssi            = out->connected ? rt_wlan_get_rssi() : 0;

    if (out->connected && rt_wlan_get_info(&info) == RT_EOK && info.ssid.len > 0)
    {
        size_t l = (info.ssid.len < 32) ? info.ssid.len : 32;
        rt_memcpy(out->ssid, info.ssid.val, l);
        out->ssid[l] = '\0';
    }
    else
    {
        rt_strncpy(out->ssid, g_wf.ssid, sizeof(out->ssid) - 1U);
    }

    wifi_get_ip_string(out->ip_str, sizeof(out->ip_str));
    rt_strncpy(out->relay_host, g_wf.relay_host, sizeof(out->relay_host) - 1U);
    rt_strncpy(out->status_msg, g_wf.status_msg, sizeof(out->status_msg) - 1U);
}

/* ----------------- MSH Commands ----------------- */

static int cmd_wifi_stream_start(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    wifi_stream_set_http_enable(RT_TRUE);
    rt_kprintf("HTTP MJPEG stream enabled on :%d\n", WIFI_STREAM_HTTP_PORT);
    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_wifi_stream_start, wifi_stream_start, Enable HTTP MJPEG stream);

static int cmd_wifi_stream_stop(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    wifi_stream_set_http_enable(RT_FALSE);
    rt_kprintf("HTTP MJPEG stream disabled\n");
    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_wifi_stream_stop, wifi_stream_stop, Disable HTTP MJPEG stream);

static int cmd_wifi_relay(int argc, char **argv)
{
    if (argc < 2)
    {
        rt_kprintf("Usage: wifi_relay <ip> [port]\n");
        return -1;
    }
    uint16_t port = (argc >= 3) ? (uint16_t)atoi(argv[2]) : WIFI_STREAM_RELAY_PORT_DEF;
    wifi_stream_set_relay_config(argv[1], port, RT_TRUE);
    wifi_stream_push_snapshot("manual_test.jpg");
    rt_kprintf("Relay target -> %s:%u (queued snapshot)\n", argv[1], (unsigned)port);
    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_wifi_relay, wifi_relay, Configure TCP JPG relay and push test frame);

static int cmd_wifi_save(int argc, char **argv)
{
    if (argc < 3)
    {
        rt_kprintf("Usage: wifi_save <ssid> <pwd> [relay_ip] [relay_port]\n");
        return -1;
    }
    const char *rhost = (argc >= 4) ? argv[3] : g_wf.relay_host;
    uint16_t rport = (argc >= 5) ? (uint16_t)atoi(argv[4]) : g_wf.relay_port;
    if (rhost != NULL && rhost[0] != '\0')
    {
        wifi_stream_set_relay_config(rhost, rport, g_wf.relay_auto_push);
    }
    wifi_save_cfg_file(argv[1], argv[2], g_wf.relay_host, g_wf.relay_port);
    wifi_mgr_connect(argv[1], argv[2], RT_FALSE);
    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_wifi_save, wifi_save, Save Wi-Fi config to /sdcard/wifi.cfg and connect);
