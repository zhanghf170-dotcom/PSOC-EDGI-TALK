/*
 * XiaoZhi AI Voice Client for Edgi-Talk Cockpit (Gemini-chan Persona)
 *
 * Implements the XiaoZhi OTA + WSS v1/v3 audio & JSON protocol with:
 *   - Non-blocking startup waiting for wlan0 IP readiness
 *   - Automatic xiaozhi.me 6-digit activation code extraction & UI display
 *   - Dynamic OTA WebSocket URL/token override + optional /sdcard/xiaozhi.cfg
 *   - Full 18-emotion mapping to 8 Gemini-chan anime expressions
 *   - Multi-turn continuous voice dialogue, tap/button interrupt & MCP cockpit control
 */

#include <rtthread.h>
#include <rtdevice.h>
#include <wlan_mgnt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cJSON.h>

extern "C" {
#include "lwip/api.h"
#include "lwip/dns.h"
#include "lwip/tcp.h"
#include "lwip/tcpip.h"
#include "lwip/apps/websocket_client.h"
#include <netdev.h>
#include <webclient.h>
#include "cy_syslib.h"
#include "xiaozhi.h"
#include "iot/iot_c_api.h"
#include "mcp/mcp_api.h"
}

#define DBG_TAG "xz.ws"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

#define XZ_DEFAULT_HOST         "api.tenclass.net"
#define XZ_DEFAULT_WSPATH       "/xiaozhi/v1/"
#define XZ_DEFAULT_TOKEN        ""
#define XZ_CFG_PATH             "/sdcard/xiaozhi.cfg"
#define XZ_TTS_BRIDGE_PORT      8890U
#define XZ_TTS_QUEUE_MAX        8
#define XZ_TTS_TEXT_MAX         256

#define XZ_MAX_WSOCK_HDR_LEN    4096
#define XZ_WS_TIMEOUT_MS        8000
#define XZ_TTS_END_TIMEOUT_MS   6000
#define XZ_TTS_RESTART_DELAY_MS 450

#define XZ_EVT_WIFI_READY       (1U << 0)
#define XZ_EVT_TRIGGER_TALK     (1U << 1)
#define XZ_EVT_STOP_CONV        (1U << 2)
#define XZ_EVT_RECONNECT        (1U << 3)
#define XZ_EVT_MULTI_TURN_NEXT  (1U << 4)
#define XZ_EVT_TTS_TIMEOUT      (1U << 5)
#define XZ_EVT_WS_TEXT          (1U << 6)
#define XZ_EVT_WS_DISCONNECTED  (1U << 7)
#define XZ_WS_RX_MB_SIZE        16U

#define XZ_HELLO_MESSAGE "{" \
    "\"type\":\"hello\"," \
    "\"version\":3," \
    "\"features\":{\"mcp\":true}," \
    "\"transport\":\"websocket\"," \
    "\"audio_params\":{" \
        "\"format\":\"opus\",\"sample_rate\":16000,\"channels\":1,\"frame_duration\":60" \
    "}}"

#define XZ_OTA_BODY_FMT \
    "{\"version\":2,\"flash_size\":16777216,\"psram_size\":2097152," \
    "\"minimum_free_heap_size\":200000,\"mac_address\":\"%s\"," \
    "\"uuid\":\"%s\",\"chip_model_name\":\"psocedge84\"," \
    "\"chip_info\":{\"model\":1,\"cores\":2,\"revision\":1,\"features\":0}," \
    "\"application\":{\"name\":\"edgi-gemini-cockpit\",\"version\":\"1.2.0\"," \
    "\"compile_time\":\"" __DATE__ "T" __TIME__ "Z\",\"idf_version\":\"rtt-5.0.2\"," \
    "\"elf_sha256\":\"\"},\"partition_table\":[{\"label\":\"app\"," \
    "\"type\":1,\"subtype\":2,\"address\":10000,\"size\":100000}]," \
    "\"ota\":{\"label\":\"ota_0\"},\"board\":{\"type\":\"edgi-talk-e84\"," \
    "\"mac\":\"%s\"}}"

typedef struct
{
    rt_bool_t       inited;
    rt_bool_t       iot_inited;
    rt_bool_t       wifi_ready;
    rt_bool_t       ws_connected;
    rt_bool_t       reconnecting;
    rt_bool_t       multi_turn;
    rt_bool_t       pending_listen;
    rt_bool_t       cloud_tts_active;
    rt_bool_t       cloud_tts_stopped;
    volatile rt_bool_t mute_cur_sentence;
    volatile rt_bool_t tts_busy;
    volatile rt_bool_t tts_abort;
    rt_bool_t       tts_bridge_ok;

    xz_state_t      state;
    xz_emotion_t    emotion;
    xz_voice_mode_t voice_mode;
    uint32_t        session_turns;
    uint32_t        ui_rev;

    char            mac_str[20];
    char            client_id[40];
    char            session_id[32];

    char            ota_host[96];
    char            ws_host[96];
    char            ws_path[96];
    char            ws_token[128];
    uint16_t        ws_port;
    uint8_t         ws_use_tls;

    char            tts_host[32];
    uint16_t        tts_port;

    char            status_text[48];
    char            emotion_name[24];
    char            user_text[160];
    char            assistant_text[256];
    char            activation_code[24];

    wsock_state_t   clnt;
    rt_sem_t        ws_sem;
    rt_mutex_t      ws_write_lock;
    struct rt_mutex state_lock;
    rt_event_t      ctrl_evt;
    rt_mailbox_t    ws_rx_mb;
    rt_mq_t         tts_mq;
    rt_timer_t      tts_end_timer;
    rt_timer_t      tts_restart_timer;
    rt_thread_t     mgr_tid;
    rt_thread_t     tts_tid;
} xz_ctx_t;

static xz_ctx_t *g_xz = RT_NULL;

/* ---------------- State / UI Helpers ---------------- */

static xz_emotion_t map_emotion_name(const char *name)
{
    if (name == RT_NULL || name[0] == '\0') return XZ_EMO_IDLE;
    if (strcmp(name, "happy") == 0 || strcmp(name, "laughing") == 0 || strcmp(name, "funny") == 0)
        return XZ_EMO_SPEAKING;
    if (strcmp(name, "winking") == 0 || strcmp(name, "loving") == 0 ||
        strcmp(name, "cool") == 0 || strcmp(name, "delicious") == 0 ||
        strcmp(name, "kissy") == 0)
        return XZ_EMO_WINKING;
    if (strcmp(name, "surprised") == 0 || strcmp(name, "shocked") == 0)
        return XZ_EMO_SURPRISED;
    if (strcmp(name, "sad") == 0 || strcmp(name, "angry") == 0 || strcmp(name, "crying") == 0)
        return XZ_EMO_SAD;
    if (strcmp(name, "thinking") == 0)
        return XZ_EMO_THINKING;
    if (strcmp(name, "sleepy") == 0)
        return XZ_EMO_SLEEPY;
    if (strcmp(name, "listening") == 0)
        return XZ_EMO_LISTENING;
    return XZ_EMO_IDLE;
}

static void xz_copy_str(char *dst, size_t cap, const char *src)
{
    if (dst == RT_NULL || cap == 0U) return;
    if (src == RT_NULL)
    {
        dst[0] = '\0';
        return;
    }
    rt_strncpy(dst, src, cap - 1U);
    dst[cap - 1U] = '\0';
}

static void xz_set_ui_state(xz_state_t st, const char *status_txt,
                            xz_emotion_t emo, const char *emo_name)
{
    if (g_xz == RT_NULL) return;
    rt_mutex_take(&g_xz->state_lock, RT_WAITING_FOREVER);
    g_xz->state = st;
    if (status_txt != RT_NULL) xz_copy_str(g_xz->status_text, sizeof(g_xz->status_text), status_txt);
    if (emo < XZ_EMO_COUNT) g_xz->emotion = emo;
    if (emo_name != RT_NULL) xz_copy_str(g_xz->emotion_name, sizeof(g_xz->emotion_name), emo_name);
    g_xz->ui_rev++;
    rt_mutex_release(&g_xz->state_lock);
}

static void xz_set_assistant_text(const char *txt)
{
    if (g_xz == RT_NULL || txt == RT_NULL) return;
    rt_mutex_take(&g_xz->state_lock, RT_WAITING_FOREVER);
    xz_copy_str(g_xz->assistant_text, sizeof(g_xz->assistant_text), txt);
    g_xz->ui_rev++;
    rt_mutex_release(&g_xz->state_lock);
}

static void xz_set_user_text(const char *txt)
{
    if (g_xz == RT_NULL || txt == RT_NULL) return;
    rt_mutex_take(&g_xz->state_lock, RT_WAITING_FOREVER);
    xz_copy_str(g_xz->user_text, sizeof(g_xz->user_text), txt);
    g_xz->ui_rev++;
    rt_mutex_release(&g_xz->state_lock);
}

static void xz_set_activation_code(const char *code)
{
    if (g_xz == RT_NULL) return;
    rt_mutex_take(&g_xz->state_lock, RT_WAITING_FOREVER);
    xz_copy_str(g_xz->activation_code, sizeof(g_xz->activation_code), code ? code : "");
    g_xz->ui_rev++;
    rt_mutex_release(&g_xz->state_lock);
}

void xiaozhi_set_emotion(xz_emotion_t emo, const char *raw_name)
{
    if (g_xz == RT_NULL || emo >= XZ_EMO_COUNT) return;
    rt_mutex_take(&g_xz->state_lock, RT_WAITING_FOREVER);
    g_xz->emotion = emo;
    if (raw_name != RT_NULL) xz_copy_str(g_xz->emotion_name, sizeof(g_xz->emotion_name), raw_name);
    g_xz->ui_rev++;
    rt_mutex_release(&g_xz->state_lock);
}

void xiaozhi_set_emotion_name(const char *emotion)
{
    if (emotion == RT_NULL) return;
    xiaozhi_set_emotion(map_emotion_name(emotion), emotion);
}

/* ---------------- Device Identity & Config ---------------- */

static const char *xz_get_mac_address(void)
{
    if (g_xz == RT_NULL) return "02:00:00:00:00:01";

    struct rt_wlan_device *sta = (struct rt_wlan_device *)rt_device_find(RT_WLAN_DEVICE_STA_NAME);
    struct netdev *nd = (sta != RT_NULL) ? sta->netdev : netdev_get_by_name("w0");
    if (nd == RT_NULL) nd = netdev_default;

    if (nd != RT_NULL && nd->hwaddr_len == 6)
    {
        rt_snprintf(g_xz->mac_str, sizeof(g_xz->mac_str),
                    "%02x:%02x:%02x:%02x:%02x:%02x",
                    nd->hwaddr[0], nd->hwaddr[1], nd->hwaddr[2],
                    nd->hwaddr[3], nd->hwaddr[4], nd->hwaddr[5]);
        return g_xz->mac_str;
    }

    if (g_xz->mac_str[0] == '\0')
    {
        uint64_t uid = Cy_SysLib_GetUniqueId();
        rt_snprintf(g_xz->mac_str, sizeof(g_xz->mac_str),
                    "02:%02x:%02x:%02x:%02x:%02x",
                    (unsigned)((uid >> 32) & 0xFFU),
                    (unsigned)((uid >> 24) & 0xFFU),
                    (unsigned)((uid >> 16) & 0xFFU),
                    (unsigned)((uid >> 8)  & 0xFFU),
                    (unsigned)(uid & 0xFFU));
    }
    return g_xz->mac_str;
}

static const char *xz_get_client_id(void)
{
    if (g_xz == RT_NULL) return "af7ac552-9991-4b31-b660-683b210ae95f";
    if (g_xz->client_id[0] == '\0')
    {
        /* Stable UUID derived from chip unique ID so xiaozhi.me binding persists across reboots */
        uint64_t uid = Cy_SysLib_GetUniqueId();
        uint32_t hi = (uint32_t)(uid >> 32);
        uint32_t lo = (uint32_t)(uid & 0xFFFFFFFFU);
        rt_snprintf(g_xz->client_id, sizeof(g_xz->client_id),
                    "%08lx-%04lx-4%03lx-a%03lx-%04lx%08lx",
                    (unsigned long)(hi ^ 0x5A3C9E17UL),
                    (unsigned long)((lo >> 16) & 0xFFFFUL),
                    (unsigned long)(lo & 0x0FFFUL),
                    (unsigned long)((hi >> 8) & 0x0FFFUL),
                    (unsigned long)((hi ^ lo) & 0xFFFFUL),
                    (unsigned long)(lo ^ 0xA5C3197EUL));
    }
    return g_xz->client_id;
}

static void xz_load_cfg_file(void)
{
    FILE *fp = fopen(XZ_CFG_PATH, "r");
    if (fp == RT_NULL) return;

    char line[160];
    while (fgets(line, sizeof(line), fp) != RT_NULL)
    {
        size_t len = strlen(line);
        while (len > 0U && (line[len - 1U] == '\r' || line[len - 1U] == '\n' || line[len - 1U] == ' '))
            line[--len] = '\0';
        char *eq = strchr(line, '=');
        if (eq == RT_NULL) continue;
        *eq = '\0';
        const char *k = line;
        const char *v = eq + 1;
        if (strcmp(k, "host") == 0 && v[0] != '\0')
        {
            xz_copy_str(g_xz->ota_host, sizeof(g_xz->ota_host), v);
            xz_copy_str(g_xz->ws_host, sizeof(g_xz->ws_host), v);
        }
        else if (strcmp(k, "path") == 0 && v[0] != '\0')
        {
            xz_copy_str(g_xz->ws_path, sizeof(g_xz->ws_path), v);
        }
        else if (strcmp(k, "token") == 0 && v[0] != '\0')
        {
            xz_copy_str(g_xz->ws_token, sizeof(g_xz->ws_token), v);
        }
        else if (strcmp(k, "port") == 0)
        {
            int p = atoi(v);
            if (p > 0 && p <= 65535) g_xz->ws_port = (uint16_t)p;
        }
        else if (strcmp(k, "tls") == 0)
        {
            g_xz->ws_use_tls = (atoi(v) != 0) ? 1U : 0U;
        }
        else if (strcmp(k, "tts_host") == 0 && v[0] != '\0')
        {
            xz_copy_str(g_xz->tts_host, sizeof(g_xz->tts_host), v);
        }
        else if (strcmp(k, "tts_port") == 0)
        {
            int p = atoi(v);
            if (p > 0 && p <= 65535) g_xz->tts_port = (uint16_t)p;
        }
    }
    fclose(fp);
    LOG_I("Loaded custom server from %s: %s:%u%s", XZ_CFG_PATH,
          g_xz->ws_host, (unsigned)g_xz->ws_port, g_xz->ws_path);
}

static void xz_parse_ws_url(const char *url)
{
    if (url == RT_NULL || g_xz == RT_NULL) return;
    const char *p = url;
    if (strncmp(p, "wss://", 6) == 0)
    {
        g_xz->ws_use_tls = 1U;
        g_xz->ws_port = 443U;
        p += 6;
    }
    else if (strncmp(p, "ws://", 5) == 0)
    {
        g_xz->ws_use_tls = 0U;
        g_xz->ws_port = 80U;
        p += 5;
    }
    else
    {
        return;
    }

    const char *slash = strchr(p, '/');
    const char *colon = strchr(p, ':');
    size_t host_len;
    if (colon != RT_NULL && (slash == RT_NULL || colon < slash))
    {
        host_len = (size_t)(colon - p);
        int port = atoi(colon + 1);
        if (port > 0 && port <= 65535) g_xz->ws_port = (uint16_t)port;
    }
    else
    {
        host_len = (slash != RT_NULL) ? (size_t)(slash - p) : strlen(p);
    }

    if (host_len > 0U && host_len < sizeof(g_xz->ws_host))
    {
        rt_memcpy(g_xz->ws_host, p, host_len);
        g_xz->ws_host[host_len] = '\0';
    }
    if (slash != RT_NULL && slash[0] != '\0')
    {
        xz_copy_str(g_xz->ws_path, sizeof(g_xz->ws_path), slash);
    }
}

/* ---------------- WebSocket Write Helpers ---------------- */

int xz_ws_send_text(const char *text, size_t len)
{
    if (g_xz == RT_NULL || !g_xz->ws_connected || g_xz->ws_write_lock == RT_NULL || text == RT_NULL)
    {
        return -RT_ERROR;
    }
    if (rt_mutex_take(g_xz->ws_write_lock, rt_tick_from_millisecond(200)) != RT_EOK)
    {
        return -RT_EBUSY;
    }
    err_t err = ERR_CLSD;
    if (g_xz->ws_connected)
    {
        err = wsock_write(&g_xz->clnt, text, len, OPCODE_TEXT);
        if (err == ERR_CLSD || err == ERR_RST)
        {
            g_xz->ws_connected = RT_FALSE;
        }
    }
    rt_mutex_release(g_xz->ws_write_lock);
    return (err == ERR_OK) ? RT_EOK : -RT_ERROR;
}

const char *xz_ws_get_session_id(void)
{
    return (g_xz != RT_NULL) ? g_xz->session_id : "";
}

void xz_audio_send_using_websocket(const uint8_t *data, int len)
{
    if (g_xz == RT_NULL || !g_xz->ws_connected || g_xz->ws_write_lock == RT_NULL ||
        data == RT_NULL || len <= 0)
    {
        return;
    }
    if (rt_mutex_take(g_xz->ws_write_lock, RT_WAITING_NO) == RT_EOK)
    {
        if (g_xz->ws_connected)
        {
            err_t err = wsock_write(&g_xz->clnt, (const char *)data, (size_t)len, OPCODE_BINARY);
            if (err == ERR_CLSD || err == ERR_RST)
            {
                g_xz->ws_connected = RT_FALSE;
            }
        }
        rt_mutex_release(g_xz->ws_write_lock);
    }
}

static rt_bool_t xz_send_listen_start(void)
{
    char msg[192];
    rt_snprintf(msg, sizeof(msg),
                "{\"session_id\":\"%s\",\"type\":\"listen\",\"state\":\"start\",\"mode\":\"auto_stop\"}",
                g_xz->session_id);
    if (xz_ws_send_text(msg, strlen(msg)) == RT_EOK)
    {
        xz_set_ui_state(XZ_STATE_LISTENING, "聆听中...", XZ_EMO_LISTENING, "listening");
        return RT_TRUE;
    }
    return RT_FALSE;
}

static void xz_send_listen_stop(void)
{
    char msg[160];
    rt_snprintf(msg, sizeof(msg),
                "{\"session_id\":\"%s\",\"type\":\"listen\",\"state\":\"stop\"}",
                g_xz->session_id);
    (void)xz_ws_send_text(msg, strlen(msg));
}

static void xz_send_abort(void)
{
    char msg[160];
    rt_snprintf(msg, sizeof(msg),
                "{\"session_id\":\"%s\",\"type\":\"abort\",\"reason\":\"wake_word_detected\"}",
                g_xz->session_id);
    (void)xz_ws_send_text(msg, strlen(msg));
}

static void xz_send_iot_descriptors(void)
{
    const char *desc = iot_get_descriptors_json();
    if (desc == RT_NULL) return;
    size_t sz = strlen(desc) + 160U;
    char *buf = (char *)rt_malloc(sz);
    if (buf == RT_NULL) return;
    rt_snprintf(buf, sz,
                "{\"session_id\":\"%s\",\"type\":\"iot\",\"update\":true,\"descriptors\":%s}",
                g_xz->session_id, desc);
    (void)xz_ws_send_text(buf, strlen(buf));
    rt_free(buf);
}

static void xz_send_iot_states(void)
{
    const char *st = iot_get_states_json();
    if (st == RT_NULL) return;
    size_t sz = strlen(st) + 160U;
    char *buf = (char *)rt_malloc(sz);
    if (buf == RT_NULL) return;
    rt_snprintf(buf, sz,
                "{\"session_id\":\"%s\",\"type\":\"iot\",\"update\":true,\"states\":%s}",
                g_xz->session_id, st);
    (void)xz_ws_send_text(buf, strlen(buf));
    rt_free(buf);
}

/* ---------------- Timers ---------------- */

static void xz_tts_end_timeout_cb(void *param)
{
    (void)param;
    if (g_xz != RT_NULL && g_xz->ctrl_evt != RT_NULL)
    {
        rt_event_send(g_xz->ctrl_evt, XZ_EVT_TTS_TIMEOUT);
    }
}

static void xz_tts_restart_cb(void *param)
{
    (void)param;
    if (g_xz != RT_NULL && g_xz->ctrl_evt != RT_NULL)
    {
        rt_event_send(g_xz->ctrl_evt, XZ_EVT_MULTI_TURN_NEXT);
    }
}

/* ---------------- LAN XiaoyiNeural Live TTS Client ---------------- */

static void xz_tts_clear_queue(void)
{
    if (g_xz == RT_NULL) return;
    g_xz->tts_abort = RT_TRUE;
    xz_audio_stream_abort();
    if (g_xz->tts_mq != RT_NULL)
    {
        char dummy[XZ_TTS_TEXT_MAX];
        while (rt_mq_recv(g_xz->tts_mq, dummy, sizeof(dummy), RT_WAITING_NO) > 0) {}
    }
}

static void xz_tts_enqueue(const char *text)
{
    if (g_xz == RT_NULL || g_xz->tts_mq == RT_NULL || text == RT_NULL || text[0] == '\0')
        return;
    char msg[XZ_TTS_TEXT_MAX];
    xz_copy_str(msg, sizeof(msg), text);
    g_xz->tts_abort = RT_FALSE;
    if (rt_mq_send(g_xz->tts_mq, msg, sizeof(msg)) != RT_EOK)
    {
        LOG_W("xz_tts queue full, dropping sentence");
    }
}

static void xz_tts_worker_entry(void *param)
{
    (void)param;
    char text[XZ_TTS_TEXT_MAX];

    while (1)
    {
        if (rt_mq_recv(g_xz->tts_mq, text, sizeof(text), RT_WAITING_FOREVER) <= 0)
            continue;

        if (g_xz->tts_abort) continue;

        g_xz->tts_busy = RT_TRUE;
        if (g_xz->state != XZ_STATE_SPEAKING)
        {
            xz_set_ui_state(XZ_STATE_SPEAKING, "说话中 · 萌妹音", XZ_EMO_SPEAKING, "happy");
        }

        uint16_t port = g_xz->tts_port ? g_xz->tts_port : XZ_TTS_BRIDGE_PORT;
        rt_bool_t ok = xz_tts_net_stream_sentence(g_xz->tts_host, sizeof(g_xz->tts_host),
                                                  port, text, &g_xz->tts_abort, &g_xz->tts_bridge_ok);
        if (!ok && !g_xz->tts_abort)
        {
            LOG_W("Xiaoyi TTS bridge unreachable on LAN :%u", (unsigned)port);
            xz_set_ui_state(g_xz->state, "萌妹音未连PC", g_xz->emotion, RT_NULL);
        }

        /* Check if more sentences are queued */
        if (g_xz->tts_mq->entry == 0U)
        {
            g_xz->tts_busy = RT_FALSE;
            if (!g_xz->tts_abort && g_xz->cloud_tts_stopped)
            {
                g_xz->cloud_tts_stopped = RT_FALSE;
                xz_set_ui_state(g_xz->ws_connected ? XZ_STATE_IDLE : XZ_STATE_SLEEPING,
                                g_xz->ws_connected ? "待命中" : "休眠中",
                                g_xz->ws_connected ? XZ_EMO_IDLE : XZ_EMO_SLEEPY,
                                g_xz->ws_connected ? "neutral" : "sleepy");
                if (g_xz->multi_turn && g_xz->ws_connected && g_xz->tts_restart_timer != RT_NULL)
                {
                    rt_timer_stop(g_xz->tts_restart_timer);
                    rt_timer_start(g_xz->tts_restart_timer);
                }
            }
            else if (!g_xz->tts_abort && !g_xz->cloud_tts_active && g_xz->state == XZ_STATE_SPEAKING)
            {
                xz_set_ui_state(XZ_STATE_IDLE, ok ? "待命中 · 萌妹音" : "萌妹音未连PC",
                                XZ_EMO_IDLE, "neutral");
            }
        }
    }
}

/* ---------------- JSON Message Handler ---------------- */

static rt_bool_t xz_is_ascii_ident_start(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

static rt_bool_t xz_is_ascii_ident_char(char c)
{
    return xz_is_ascii_ident_start(c) || (c >= '0' && c <= '9') ||
           c == '.' || c == ':' || c == '/' || c == '-';
}

static const char *xz_skip_paired_delim(const char *p, char open_ch, char close_ch)
{
    int depth = 0;
    while (*p != '\0')
    {
        if (*p == open_ch) depth++;
        else if (*p == close_ch)
        {
            depth--;
            if (depth <= 0) return p + 1;
        }
        else if (*p == '\r' || *p == '\n')
        {
            break;
        }
        p++;
    }
    return RT_NULL;
}

/* Filter server/LLM intent & function-call tags such as "%getweather",
 * "%play_music(...)", "<tool_call>...</tool_call>", or "<|...|>" while
 * preserving real numbers/percentages like "46%". */
static rt_bool_t xz_sanitize_assistant_text(const char *raw, char *out, size_t cap)
{
    if (out == RT_NULL || cap == 0U) return RT_FALSE;
    out[0] = '\0';
    if (raw == RT_NULL || raw[0] == '\0') return RT_FALSE;

    size_t w = 0U;
    const char *p = raw;
    rt_bool_t stripped_head = RT_FALSE;

    while (*p != '\0' && w + 1U < cap)
    {
        /* Strip %command / %getweather(...) style plugin tags */
        if (*p == '%' && xz_is_ascii_ident_start(p[1]))
        {
            const char *q = p + 2;
            while (xz_is_ascii_ident_char(*q)) q++;
            if (*q == '(')
            {
                const char *after = xz_skip_paired_delim(q, '(', ')');
                if (after != RT_NULL) q = after;
            }
            else if (*q == '[')
            {
                const char *after = xz_skip_paired_delim(q, '[', ']');
                if (after != RT_NULL) q = after;
            }
            else if (*q == '{')
            {
                const char *after = xz_skip_paired_delim(q, '{', '}');
                if (after != RT_NULL) q = after;
            }
            if (w == 0U) stripped_head = RT_TRUE;
            p = q;
            while (*p == ' ' || *p == '\t') p++;
            continue;
        }

        /* Strip <|...|> special model tokens */
        if (p[0] == '<' && p[1] == '|')
        {
            const char *end = strstr(p + 2, "|>");
            if (end != RT_NULL)
            {
                if (w == 0U) stripped_head = RT_TRUE;
                p = end + 2;
                continue;
            }
        }

        /* Strip <tool_call>...</tool_call> or <function...>...</function> blocks */
        if (strncmp(p, "<tool_call", 10) == 0)
        {
            const char *end = strstr(p + 10, "</tool_call>");
            p = (end != RT_NULL) ? (end + 12) : (p + strlen(p));
            if (w == 0U) stripped_head = RT_TRUE;
            continue;
        }
        if (strncmp(p, "<function", 9) == 0)
        {
            const char *end = strstr(p + 9, "</function>");
            p = (end != RT_NULL) ? (end + 11) : (p + strlen(p));
            if (w == 0U) stripped_head = RT_TRUE;
            continue;
        }

        /* Skip leading whitespace (and stray ASCII/CJK colons/commas left right after a stripped prefix tag) */
        if (w == 0U)
        {
            if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
            {
                p++;
                continue;
            }
            if (stripped_head && (*p == ':' || *p == ';' || *p == ',' || *p == '-'))
            {
                p++;
                continue;
            }
            if (stripped_head && (unsigned char)p[0] == 0xEFU && (unsigned char)p[1] == 0xBCU &&
                ((unsigned char)p[2] == 0x9AU || (unsigned char)p[2] == 0x8CU))
            {
                /* UTF-8 fullwidth '：' (EF BC 9A) or '，' (EF BC 8C) */
                p += 3;
                continue;
            }
        }

        out[w++] = *p++;
    }
    out[w] = '\0';

    while (w > 0U && (out[w - 1U] == ' ' || out[w - 1U] == '\t' ||
                      out[w - 1U] == '\r' || out[w - 1U] == '\n'))
    {
        out[--w] = '\0';
    }

    /* Require at least one speakable alphanumeric or non-ASCII (CJK) byte */
    for (size_t i = 0U; i < w; i++)
    {
        unsigned char c = (unsigned char)out[i];
        if (c >= 0x80U || (c >= '0' && c <= '9') ||
            (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))
        {
            return RT_TRUE;
        }
    }
    out[0] = '\0';
    return RT_FALSE;
}

static const char *json_str_or_empty(const cJSON *obj, const char *key)
{
    const cJSON *item = cJSON_GetObjectItem(obj, key);
    if (item != RT_NULL && cJSON_IsString(item) && item->valuestring != RT_NULL)
    {
        return item->valuestring;
    }
    return "";
}

static void xz_handle_text_message_str(const char *json_str)
{
    if (json_str == RT_NULL) return;

    cJSON *root = cJSON_Parse(json_str);
    if (root == RT_NULL)
    {
        LOG_W("JSON parse error");
        return;
    }

    const char *type = json_str_or_empty(root, "type");

    if (strcmp(type, "hello") == 0)
    {
        const char *sid = json_str_or_empty(root, "session_id");
        xz_copy_str(g_xz->session_id, sizeof(g_xz->session_id), sid);
        LOG_I("XiaoZhi hello OK (session=%s)", g_xz->session_id);

        if (!g_xz->iot_inited)
        {
            iot_initialize();
            g_xz->iot_inited = RT_TRUE;
        }
        xz_send_iot_descriptors();
        xz_send_iot_states();

        if (g_xz->pending_listen)
        {
            g_xz->pending_listen = RT_FALSE;
            rt_event_send(g_xz->ctrl_evt, XZ_EVT_TRIGGER_TALK);
        }
        else if (g_xz->activation_code[0] != '\0')
        {
            xz_set_ui_state(XZ_STATE_ACTIVATING, "等待绑定设备", XZ_EMO_WINKING, "winking");
        }
        else
        {
            xz_set_ui_state(XZ_STATE_IDLE, "待命中 · 点击对话", XZ_EMO_IDLE, "neutral");
            if (g_xz->assistant_text[0] == '\0')
            {
                xz_set_assistant_text("主人好，我是 Gemini，点击头像或按 SW3(P8.3) 就可以说话了喵。");
            }
        }
    }
    else if (strcmp(type, "goodbye") == 0)
    {
        xz_tts_clear_queue();
        xz_mic(0);
        xz_speaker(0);
        if (g_xz->tts_end_timer != RT_NULL) rt_timer_stop(g_xz->tts_end_timer);
        if (g_xz->tts_restart_timer != RT_NULL) rt_timer_stop(g_xz->tts_restart_timer);
        xz_set_ui_state(XZ_STATE_SLEEPING, "休眠中 · 点击唤醒", XZ_EMO_SLEEPY, "sleepy");
        xz_set_assistant_text("先稍微休息一下，有需要随时叫醒我喵。");
        LOG_I("Session goodbye");
    }
    else if (strcmp(type, "stt") == 0)
    {
        const char *text = json_str_or_empty(root, "text");
        LOG_I("STT: %s", text);
        if (text[0] != '\0')
        {
            xz_set_user_text(text);
            if (g_xz->state == XZ_STATE_LISTENING)
            {
                xz_mic(0);
                xz_set_ui_state(XZ_STATE_THINKING, "思考中...", XZ_EMO_THINKING, "thinking");
            }
        }
    }
    else if (strcmp(type, "tts") == 0)
    {
        const char *st = json_str_or_empty(root, "state");
        if (strcmp(st, "start") == 0)
        {
            xz_mic(0);
            if (g_xz->tts_restart_timer != RT_NULL) rt_timer_stop(g_xz->tts_restart_timer);
            g_xz->session_turns++;
            g_xz->cloud_tts_active  = RT_TRUE;
            g_xz->cloud_tts_stopped = RT_FALSE;
            g_xz->mute_cur_sentence = RT_FALSE;
            if (g_xz->voice_mode == XZ_VOICE_MODE_CLOUD)
            {
                xz_speaker(1);
                xz_set_ui_state(XZ_STATE_SPEAKING, "说话中 · 云端音", XZ_EMO_SPEAKING, "happy");
            }
            else
            {
                /* Cute XiaoyiNeural voice mode: mute cloud Opus & synthesize sentence text */
                g_xz->tts_abort = RT_FALSE;
                xz_set_ui_state(XZ_STATE_SPEAKING, "说话中 · 萌妹音", XZ_EMO_SPEAKING, "happy");
            }
            if (g_xz->voice_mode == XZ_VOICE_MODE_CLOUD && g_xz->multi_turn && g_xz->tts_end_timer != RT_NULL)
            {
                rt_timer_stop(g_xz->tts_end_timer);
                rt_timer_start(g_xz->tts_end_timer);
            }
        }
        else if (strcmp(st, "sentence_start") == 0)
        {
            const char *text = json_str_or_empty(root, "text");
            char clean[XZ_TTS_TEXT_MAX];
            if (xz_sanitize_assistant_text(text, clean, sizeof(clean)))
            {
                g_xz->mute_cur_sentence = RT_FALSE;
                LOG_I("TTS: %s", clean);
                xz_set_assistant_text(clean);
                if (g_xz->voice_mode == XZ_VOICE_MODE_XIAOYI)
                {
                    xz_tts_enqueue(clean);
                }
            }
            else if (text[0] != '\0')
            {
                g_xz->mute_cur_sentence = RT_TRUE;
                LOG_I("TTS filtered control tag: %s", text);
            }
            if (g_xz->voice_mode == XZ_VOICE_MODE_CLOUD && g_xz->multi_turn && g_xz->tts_end_timer != RT_NULL)
            {
                rt_timer_stop(g_xz->tts_end_timer);
                rt_timer_start(g_xz->tts_end_timer);
            }
        }
        else if (strcmp(st, "sentence_end") == 0)
        {
            g_xz->mute_cur_sentence = RT_FALSE;
            if (g_xz->voice_mode == XZ_VOICE_MODE_CLOUD &&
                g_xz->multi_turn && g_xz->state == XZ_STATE_SPEAKING && g_xz->tts_end_timer != RT_NULL)
            {
                rt_timer_stop(g_xz->tts_end_timer);
                rt_timer_start(g_xz->tts_end_timer);
            }
        }
        else if (strcmp(st, "stop") == 0)
        {
            if (g_xz->tts_end_timer != RT_NULL) rt_timer_stop(g_xz->tts_end_timer);
            xz_mic(0);
            g_xz->cloud_tts_active = RT_FALSE;
            g_xz->mute_cur_sentence = RT_FALSE;

            if (g_xz->voice_mode == XZ_VOICE_MODE_XIAOYI &&
                (g_xz->tts_busy || (g_xz->tts_mq != RT_NULL && g_xz->tts_mq->entry > 0U)))
            {
                /* Wait for LAN Xiaoyi voice to finish speaking before entering next listen turn */
                g_xz->cloud_tts_stopped = RT_TRUE;
            }
            else
            {
                xz_speaker(0);
                xz_set_ui_state(XZ_STATE_IDLE, "待命中", XZ_EMO_IDLE, "neutral");

                if (g_xz->multi_turn && g_xz->ws_connected && g_xz->tts_restart_timer != RT_NULL)
                {
                    rt_timer_stop(g_xz->tts_restart_timer);
                    rt_timer_start(g_xz->tts_restart_timer);
                }
            }
        }
    }
    else if (strcmp(type, "llm") == 0)
    {
        const char *emo = json_str_or_empty(root, "emotion");
        if (emo[0] != '\0')
        {
            LOG_I("LLM emotion: %s", emo);
            xiaozhi_set_emotion_name(emo);
        }
    }
    else if (strcmp(type, "iot") == 0)
    {
        cJSON *commands = cJSON_GetObjectItem(root, "commands");
        if (cJSON_IsArray(commands))
        {
            int n = cJSON_GetArraySize(commands);
            for (int i = 0; i < n; i++)
            {
                cJSON *cmd = cJSON_GetArrayItem(commands, i);
                char *cmd_str = cJSON_PrintUnformatted(cmd);
                if (cmd_str != RT_NULL)
                {
                    iot_invoke((const uint8_t *)cmd_str, (uint16_t)strlen(cmd_str));
                    xz_send_iot_states();
                    cJSON_free(cmd_str);
                }
            }
        }
    }
    else if (strcmp(type, "mcp") == 0)
    {
        cJSON *payload = cJSON_GetObjectItem(root, "payload");
        if (payload != RT_NULL && cJSON_IsObject(payload))
        {
            char *pstr = cJSON_PrintUnformatted(payload);
            if (pstr != RT_NULL)
            {
                McpServer_ParseMessage(pstr);
                cJSON_free(pstr);
            }
        }
    }
    else if (strcmp(type, "error") == 0)
    {
        const char *msg = json_str_or_empty(root, "message");
        LOG_E("XiaoZhi server error: %s", msg);
        if (msg[0] != '\0') xz_set_assistant_text(msg);
        xz_set_ui_state(XZ_STATE_ERROR, "服务异常", XZ_EMO_SAD, "sad");
    }

    cJSON_Delete(root);
}

/* ---------------- lwIP WebSocket Callback ---------------- */

/* Runs inside tcpip_thread holding lock_tcpip_core: MUST stay non-blocking
 * and NEVER acquire ws_write_lock, state_lock, or xiaozhi_audio_lock. */
static err_t xz_ws_callback(int code, char *buf, size_t len)
{
    if (g_xz == RT_NULL) return ERR_OK;

    switch (code)
    {
    case WS_CONNECT:
        if ((uint16_t)(uintptr_t)buf == 101U)
        {
            g_xz->ws_connected = RT_TRUE;
            if (g_xz->ws_sem != RT_NULL) rt_sem_release(g_xz->ws_sem);
        }
        break;

    case WS_DISCONNECT:
        if (g_xz->reconnecting) break;
        if (g_xz->ws_connected)
        {
            g_xz->ws_connected = RT_FALSE;
            if (g_xz->ctrl_evt != RT_NULL)
            {
                rt_event_send(g_xz->ctrl_evt, XZ_EVT_WS_DISCONNECTED);
            }
        }
        break;

    case WS_TEXT:
        if (buf != RT_NULL && len > 0U && g_xz->ws_rx_mb != RT_NULL && g_xz->ctrl_evt != RT_NULL)
        {
            char *copy = (char *)rt_malloc(len + 1U);
            if (copy != RT_NULL)
            {
                rt_memcpy(copy, buf, len);
                copy[len] = '\0';
                if (rt_mb_send(g_xz->ws_rx_mb, (rt_ubase_t)copy) == RT_EOK)
                {
                    rt_event_send(g_xz->ctrl_evt, XZ_EVT_WS_TEXT);
                }
                else
                {
                    rt_free(copy);
                }
            }
        }
        break;

    case WS_DATA:
        if (buf != RT_NULL && len > 0U &&
            g_xz->voice_mode == XZ_VOICE_MODE_CLOUD && !g_xz->mute_cur_sentence)
        {
            xz_audio_downlink((const uint8_t *)buf, (uint32_t)len);
        }
        break;

    default:
        break;
    }

    return ERR_OK;
}

static void xz_ws_drain_rx_mb(rt_bool_t process)
{
    if (g_xz == RT_NULL || g_xz->ws_rx_mb == RT_NULL) return;
    rt_ubase_t val = 0U;
    while (rt_mb_recv(g_xz->ws_rx_mb, &val, RT_WAITING_NO) == RT_EOK)
    {
        char *msg = (char *)(uintptr_t)val;
        if (msg != RT_NULL)
        {
            if (process)
            {
                xz_handle_text_message_str(msg);
            }
            rt_free(msg);
        }
    }
}

/* ---------------- OTA & WebSocket Connect ---------------- */

static void xz_dns_cb(const char *name, const ip_addr_t *ipaddr, void *arg)
{
    (void)name;
    (void)ipaddr;
    (void)arg;
}

static rt_bool_t xz_check_dns(const char *host)
{
    ip_addr_t addr;
    rt_memset(&addr, 0, sizeof(addr));
    LOCK_TCPIP_CORE();
    err_t err = dns_gethostbyname(host, &addr, xz_dns_cb, RT_NULL);
    UNLOCK_TCPIP_CORE();
    return (err == ERR_OK || err == ERR_INPROGRESS) ? RT_TRUE : RT_FALSE;
}

static void xz_close_ws_safe(void)
{
    if (g_xz == RT_NULL) return;
    g_xz->ws_connected = RT_FALSE;
    LOCK_TCPIP_CORE();
    if (g_xz->clnt.pcb != RT_NULL || g_xz->clnt.pconf != RT_NULL ||
        g_xz->clnt.request != RT_NULL || g_xz->clnt.state0 == PWS_STATE_INITD)
    {
        (void)wsock_close(&g_xz->clnt, WSOCK_RESULT_LOCAL_ABORT, ERR_OK);
    }
    rt_memset(&g_xz->clnt, 0, sizeof(wsock_state_t));
    UNLOCK_TCPIP_CORE();
}

static void xz_fetch_ota_info(void)
{
    if (!rt_wlan_is_ready()) return;

    char url[192];
    rt_snprintf(url, sizeof(url), "https://%s/xiaozhi/ota/", g_xz->ota_host);

    const char *mac = xz_get_mac_address();
    const char *cid = xz_get_client_id();

    char *body = (char *)rt_malloc(768);
    char *resp = (char *)rt_calloc(1, 2048);
    struct webclient_session *session = webclient_session_create(1024);

    if (body == RT_NULL || resp == RT_NULL || session == RT_NULL)
    {
        if (body) rt_free(body);
        if (resp) rt_free(resp);
        if (session) webclient_close(session);
        return;
    }

    int blen = rt_snprintf(body, 768, XZ_OTA_BODY_FMT, mac, cid, mac);
    webclient_header_fields_add(session, "Device-Id: %s\r\n", mac);
    webclient_header_fields_add(session, "Client-Id: %s\r\n", cid);
    webclient_header_fields_add(session, "Content-Type: application/json\r\n");
    webclient_header_fields_add(session, "Content-Length: %d\r\n", blen);

    int status = webclient_post(session, url, body, (size_t)blen);
    LOG_I("OTA POST %s -> HTTP %d (MAC=%s)", url, status, mac);

    if (status == 200)
    {
        int total = 0;
        int clen = webclient_content_length_get(session);
        int limit = (clen > 0 && clen < 2047) ? clen : 2047;
        while (total < limit)
        {
            int n = webclient_read(session, resp + total, (size_t)(limit - total));
            if (n <= 0) break;
            total += n;
        }
        resp[total] = '\0';

        cJSON *root = cJSON_Parse(resp);
        if (root != RT_NULL)
        {
            /* Parse dynamic WebSocket URL/token if returned by server */
            cJSON *ws = cJSON_GetObjectItem(root, "websocket");
            if (cJSON_IsObject(ws))
            {
                const char *wurl = json_str_or_empty(ws, "url");
                const char *wtok = json_str_or_empty(ws, "token");
                if (wurl[0] != '\0') xz_parse_ws_url(wurl);
                if (wtok[0] != '\0')
                {
                    if (strncmp(wtok, "Bearer ", 7) == 0)
                        xz_copy_str(g_xz->ws_token, sizeof(g_xz->ws_token), wtok);
                    else
                        rt_snprintf(g_xz->ws_token, sizeof(g_xz->ws_token), "Bearer %s", wtok);
                }
            }

            /* Parse xiaozhi.me 6-digit binding code if device is not yet bound */
            cJSON *act = cJSON_GetObjectItem(root, "activation");
            if (cJSON_IsObject(act))
            {
                const char *code = json_str_or_empty(act, "code");
                if (code[0] != '\0')
                {
                    LOG_I("XiaoZhi activation code: %s", code);
                    xz_set_activation_code(code);
                    char tip[192];
                    rt_snprintf(tip, sizeof(tip),
                                "请前往 xiaozhi.me 绑定设备，验证码：%s，绑好后点「重连」就能语音聊天了喵。",
                                code);
                    xz_set_assistant_text(tip);
                }
            }
            else
            {
                xz_set_activation_code("");
            }

            cJSON_Delete(root);
        }
    }

    webclient_close(session);
    rt_free(resp);
    rt_free(body);
}

static rt_bool_t xz_connect_websocket(void)
{
    if (!rt_wlan_is_ready())
    {
        xz_set_ui_state(XZ_STATE_OFFLINE, "未连 Wi-Fi", XZ_EMO_SLEEPY, "sleepy");
        return RT_FALSE;
    }

    g_xz->reconnecting = RT_TRUE;
    xz_set_ui_state(XZ_STATE_CONNECTING, "连接小智中...", XZ_EMO_THINKING, "thinking");

    /* Close any existing WebSocket cleanly under tcpip core lock */
    xz_ws_drain_rx_mb(RT_FALSE);
    xz_close_ws_safe();
    rt_thread_mdelay(150);

    /* Warm up DNS & fetch OTA config/activation code */
    (void)xz_check_dns(g_xz->ota_host);
    xz_fetch_ota_info();

    for (int attempt = 1; attempt <= 3; attempt++)
    {
        if (!rt_wlan_is_ready()) break;

        while (rt_sem_trytake(g_xz->ws_sem) == RT_EOK) {}
        xz_close_ws_safe();

        LOCK_TCPIP_CORE();
        err_t init_err = wsock_init(&g_xz->clnt, g_xz->ws_use_tls ? 1 : 0, 1, xz_ws_callback);
        err_t err = init_err;
        if (init_err == ERR_OK)
        {
            err = wsock_connect(&g_xz->clnt, XZ_MAX_WSOCK_HDR_LEN,
                                g_xz->ws_host, g_xz->ws_path,
                                g_xz->ws_port, g_xz->ws_token, RT_NULL,
                                "Protocol-Version: 1\r\nDevice-Id: %s\r\nClient-Id: %s\r\n",
                                xz_get_mac_address(), xz_get_client_id());
        }
        UNLOCK_TCPIP_CORE();
        LOG_I("WSS connect #%d -> %s:%u%s (err=%d)",
              attempt, g_xz->ws_host, (unsigned)g_xz->ws_port, g_xz->ws_path, (int)err);

        if (err == ERR_OK &&
            rt_sem_take(g_xz->ws_sem, rt_tick_from_millisecond(XZ_WS_TIMEOUT_MS)) == RT_EOK &&
            g_xz->ws_connected)
        {
            g_xz->reconnecting = RT_FALSE;
            if (xz_ws_send_text(XZ_HELLO_MESSAGE, strlen(XZ_HELLO_MESSAGE)) == RT_EOK)
            {
                LOG_I("XiaoZhi WebSocket ready");
                return RT_TRUE;
            }
            g_xz->reconnecting = RT_TRUE;
        }

        /* Always release timed-out/failed altcp_tls PCB before retry to prevent leak & assert */
        xz_close_ws_safe();
        rt_thread_mdelay(1200);
    }

    g_xz->reconnecting = RT_FALSE;
    g_xz->ws_connected = RT_FALSE;
    xz_set_ui_state(XZ_STATE_ERROR, "连接失败 · 点重试", XZ_EMO_SAD, "sad");
    xz_set_assistant_text("暂时连不上小智服务器呢，检查一下 Wi-Fi 或点「重连」试试喵。");
    return RT_FALSE;
}

/* ---------------- Manager Thread ---------------- */

static void xz_manager_thread_entry(void *param)
{
    (void)param;

    /* Initialize audio pipeline once (does NOT open mic0 or sound0 until needed) */
    xz_audio_init_system();

    /* Brief wait for SD card mount to pick up optional /sdcard/xiaozhi.cfg */
    rt_thread_mdelay(1800);
    xz_load_cfg_file();

    /* Check if Wi-Fi already came up during boot */
    if (rt_wlan_is_ready())
    {
        g_xz->wifi_ready = RT_TRUE;
        (void)xz_connect_websocket();
    }

    while (1)
    {
        rt_uint32_t evt = 0U;
        if (rt_event_recv(g_xz->ctrl_evt,
                          XZ_EVT_WIFI_READY | XZ_EVT_TRIGGER_TALK | XZ_EVT_STOP_CONV |
                          XZ_EVT_RECONNECT | XZ_EVT_MULTI_TURN_NEXT | XZ_EVT_TTS_TIMEOUT |
                          XZ_EVT_WS_TEXT | XZ_EVT_WS_DISCONNECTED,
                          RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR,
                          rt_tick_from_millisecond(2000), &evt) != RT_EOK)
        {
            /* Periodic Wi-Fi online polling fallback */
            rt_bool_t ready = rt_wlan_is_ready();
            if (ready && !g_xz->wifi_ready)
            {
                g_xz->wifi_ready = RT_TRUE;
                (void)xz_connect_websocket();
            }
            else if (!ready && g_xz->wifi_ready)
            {
                g_xz->wifi_ready = RT_FALSE;
                xz_close_ws_safe();
                xz_mic(0);
                xz_speaker(0);
                xz_set_ui_state(XZ_STATE_OFFLINE, "Wi-Fi 已断开", XZ_EMO_SLEEPY, "sleepy");
            }
            continue;
        }

        if (evt & XZ_EVT_WS_TEXT)
        {
            xz_ws_drain_rx_mb(RT_TRUE);
        }

        if (evt & XZ_EVT_WS_DISCONNECTED)
        {
            xz_ws_drain_rx_mb(RT_FALSE);
            if (!g_xz->ws_connected && !g_xz->reconnecting)
            {
                xz_mic(0);
                xz_speaker(0);
                if (g_xz->tts_end_timer != RT_NULL) rt_timer_stop(g_xz->tts_end_timer);
                if (g_xz->tts_restart_timer != RT_NULL) rt_timer_stop(g_xz->tts_restart_timer);
                xz_set_ui_state(XZ_STATE_SLEEPING, "休眠中 · 点击唤醒", XZ_EMO_SLEEPY, "sleepy");
                LOG_I("WebSocket disconnected -> sleeping");
            }
        }

        if (evt & XZ_EVT_WIFI_READY)
        {
            g_xz->wifi_ready = rt_wlan_is_ready();
            if (g_xz->wifi_ready && !g_xz->ws_connected)
            {
                (void)xz_connect_websocket();
            }
        }

        if (evt & XZ_EVT_RECONNECT)
        {
            g_xz->wifi_ready = rt_wlan_is_ready();
            if (g_xz->wifi_ready)
            {
                (void)xz_connect_websocket();
            }
            else
            {
                xz_set_ui_state(XZ_STATE_OFFLINE, "请先连接 Wi-Fi", XZ_EMO_SLEEPY, "sleepy");
                xz_set_assistant_text("网络好像还没连上呢，先去「无线网络」页连一下吧喵。");
            }
        }

        if (evt & XZ_EVT_STOP_CONV)
        {
            g_xz->pending_listen = RT_FALSE;
            xz_tts_clear_queue();
            if (g_xz->tts_end_timer != RT_NULL) rt_timer_stop(g_xz->tts_end_timer);
            if (g_xz->tts_restart_timer != RT_NULL) rt_timer_stop(g_xz->tts_restart_timer);
            if (g_xz->state == XZ_STATE_LISTENING) xz_send_listen_stop();
            if (g_xz->state == XZ_STATE_SPEAKING)  xz_send_abort();
            xz_mic(0);
            xz_speaker(0);
            xz_set_ui_state(g_xz->ws_connected ? XZ_STATE_IDLE : XZ_STATE_SLEEPING,
                            g_xz->ws_connected ? "待命中" : "休眠中",
                            g_xz->ws_connected ? XZ_EMO_IDLE : XZ_EMO_SLEEPY,
                            g_xz->ws_connected ? "neutral" : "sleepy");
        }

        if (evt & XZ_EVT_TRIGGER_TALK)
        {
            g_xz->wifi_ready = rt_wlan_is_ready();
            if (!g_xz->wifi_ready)
            {
                /* Offline friendly cute-voice interactive response */
                xiaozhi_cycle_demo_emotion();
                continue;
            }

            /* If local voice is speaking, stop it cleanly before starting cloud talk */
            if (xz_audio_is_local_voice_playing())
            {
                xz_audio_stop_local_voice();
                rt_thread_mdelay(40);
            }

            if (!g_xz->ws_connected)
            {
                g_xz->pending_listen = RT_TRUE;
                xz_play_wake_sound();
                (void)xz_connect_websocket();
                continue;
            }

            /* If already listening, pressing again finishes speech and asks server to respond */
            if (g_xz->state == XZ_STATE_LISTENING)
            {
                xz_mic(0);
                xz_send_listen_stop();
                xz_set_ui_state(XZ_STATE_THINKING, "思考中...", XZ_EMO_THINKING, "thinking");
                continue;
            }

            /* If currently speaking (cloud or Xiaoyi LAN TTS), interrupt and start listening */
            if (g_xz->state == XZ_STATE_SPEAKING || g_xz->tts_busy)
            {
                if (g_xz->tts_end_timer != RT_NULL) rt_timer_stop(g_xz->tts_end_timer);
                if (g_xz->tts_restart_timer != RT_NULL) rt_timer_stop(g_xz->tts_restart_timer);
                xz_tts_clear_queue();
                xz_speaker(0);
                xz_send_abort();
                rt_thread_mdelay(60);
            }

            xz_play_wake_sound();
            rt_thread_mdelay(120);
            xz_set_user_text("正在聆听你的声音...");
            xz_mic(1);
            if (!xz_send_listen_start())
            {
                xz_mic(0);
                xz_set_ui_state(XZ_STATE_ERROR, "发送失败 · 点重试", XZ_EMO_SAD, "sad");
            }
        }

        if ((evt & XZ_EVT_MULTI_TURN_NEXT) || (evt & XZ_EVT_TTS_TIMEOUT))
        {
            if (g_xz->multi_turn && g_xz->ws_connected &&
                (g_xz->state == XZ_STATE_IDLE || g_xz->state == XZ_STATE_SPEAKING))
            {
                if (g_xz->state == XZ_STATE_SPEAKING) xz_speaker(0);
                xz_mic(1);
                if (!xz_send_listen_start())
                {
                    xz_mic(0);
                    xz_set_ui_state(XZ_STATE_IDLE, "待命中", XZ_EMO_IDLE, "neutral");
                }
            }
        }
    }
}

/* ---------------- Public API ---------------- */

int xiaozhi_init(void)
{
    if (g_xz != RT_NULL && g_xz->inited) return RT_EOK;

    g_xz = (xz_ctx_t *)rt_calloc(1, sizeof(xz_ctx_t));
    if (g_xz == RT_NULL)
    {
        LOG_E("alloc xz_ctx_t failed");
        return -RT_ENOMEM;
    }

    rt_mutex_init(&g_xz->state_lock, "xz_st", RT_IPC_FLAG_PRIO);
    g_xz->ws_sem        = rt_sem_create("xz_ws", 0, RT_IPC_FLAG_FIFO);
    g_xz->ws_write_lock = rt_mutex_create("xz_wlk", RT_IPC_FLAG_PRIO);
    g_xz->ctrl_evt      = rt_event_create("xz_ctl", RT_IPC_FLAG_FIFO);
    g_xz->ws_rx_mb      = rt_mb_create("xz_rxmb", XZ_WS_RX_MB_SIZE, RT_IPC_FLAG_FIFO);
    g_xz->tts_mq        = rt_mq_create("xz_tts", XZ_TTS_TEXT_MAX, XZ_TTS_QUEUE_MAX, RT_IPC_FLAG_FIFO);
    g_xz->tts_end_timer = rt_timer_create("xz_tend", xz_tts_end_timeout_cb, RT_NULL,
                                          rt_tick_from_millisecond(XZ_TTS_END_TIMEOUT_MS),
                                          RT_TIMER_FLAG_ONE_SHOT);
    g_xz->tts_restart_timer = rt_timer_create("xz_trst", xz_tts_restart_cb, RT_NULL,
                                              rt_tick_from_millisecond(XZ_TTS_RESTART_DELAY_MS),
                                              RT_TIMER_FLAG_ONE_SHOT);

    g_xz->multi_turn = RT_TRUE;
    g_xz->voice_mode = XZ_VOICE_MODE_XIAOYI; /* Default to cute XiaoyiNeural female voice! */
    g_xz->tts_port   = XZ_TTS_BRIDGE_PORT;
    g_xz->state      = XZ_STATE_OFFLINE;
    g_xz->emotion    = XZ_EMO_IDLE;
    g_xz->ws_port    = 443U;
    g_xz->ws_use_tls = 1U;

    xz_copy_str(g_xz->ota_host, sizeof(g_xz->ota_host), XZ_DEFAULT_HOST);
    xz_copy_str(g_xz->ws_host,  sizeof(g_xz->ws_host),  XZ_DEFAULT_HOST);
    xz_copy_str(g_xz->ws_path,  sizeof(g_xz->ws_path),  XZ_DEFAULT_WSPATH);
    xz_copy_str(g_xz->ws_token, sizeof(g_xz->ws_token), XZ_DEFAULT_TOKEN);
    xz_copy_str(g_xz->status_text, sizeof(g_xz->status_text), "等待 Wi-Fi 联网");
    xz_copy_str(g_xz->emotion_name, sizeof(g_xz->emotion_name), "neutral");
    xz_copy_str(g_xz->assistant_text, sizeof(g_xz->assistant_text),
                "主人好，我是 Gemini，你的双子星猫娘助手，连上 Wi-Fi 就能语音聊天了喵。");

    g_xz->mgr_tid = rt_thread_create("xz_mgr", xz_manager_thread_entry, RT_NULL,
                                     16 * 1024, 15, 10);
    if (g_xz->mgr_tid != RT_NULL)
    {
        rt_thread_startup(g_xz->mgr_tid);
    }

    g_xz->tts_tid = rt_thread_create("xz_tts", xz_tts_worker_entry, RT_NULL,
                                     20 * 1024, 9, 10);
    if (g_xz->tts_tid != RT_NULL)
    {
        rt_thread_startup(g_xz->tts_tid);
    }

    xz_tts_net_init(g_xz->tts_port);

    g_xz->inited = RT_TRUE;
    LOG_I("XiaoZhi AI (Gemini-chan) initialized");
    return RT_EOK;
}

void xiaozhi_on_wifi_ready(void)
{
    if (g_xz != RT_NULL && g_xz->ctrl_evt != RT_NULL)
    {
        rt_event_send(g_xz->ctrl_evt, XZ_EVT_WIFI_READY);
    }
}

void xiaozhi_trigger_talk(void)
{
    if (g_xz != RT_NULL && g_xz->ctrl_evt != RT_NULL)
    {
        rt_event_send(g_xz->ctrl_evt, XZ_EVT_TRIGGER_TALK);
    }
}

void xiaozhi_stop_conversation(void)
{
    if (g_xz != RT_NULL && g_xz->ctrl_evt != RT_NULL)
    {
        rt_event_send(g_xz->ctrl_evt, XZ_EVT_STOP_CONV);
    }
}

void xiaozhi_reconnect(void)
{
    if (g_xz != RT_NULL && g_xz->ctrl_evt != RT_NULL)
    {
        rt_event_send(g_xz->ctrl_evt, XZ_EVT_RECONNECT);
    }
}

void xiaozhi_set_multi_turn(rt_bool_t enable)
{
    if (g_xz == RT_NULL) return;
    rt_mutex_take(&g_xz->state_lock, RT_WAITING_FOREVER);
    g_xz->multi_turn = enable;
    g_xz->ui_rev++;
    rt_mutex_release(&g_xz->state_lock);
}

rt_bool_t xiaozhi_get_multi_turn(void)
{
    return (g_xz != RT_NULL) ? g_xz->multi_turn : RT_FALSE;
}

void xiaozhi_set_voice_mode(xz_voice_mode_t mode)
{
    if (g_xz == RT_NULL) return;
    rt_mutex_take(&g_xz->state_lock, RT_WAITING_FOREVER);
    g_xz->voice_mode = mode;
    g_xz->ui_rev++;
    rt_mutex_release(&g_xz->state_lock);
    LOG_I("Voice mode switched -> %s", mode == XZ_VOICE_MODE_XIAOYI ? "XIAOYI (cute female)" : "CLOUD");
}

xz_voice_mode_t xiaozhi_get_voice_mode(void)
{
    return (g_xz != RT_NULL) ? g_xz->voice_mode : XZ_VOICE_MODE_XIAOYI;
}

void xiaozhi_set_tts_host(const char *ip)
{
    if (g_xz == RT_NULL || ip == RT_NULL) return;
    rt_mutex_take(&g_xz->state_lock, RT_WAITING_FOREVER);
    xz_copy_str(g_xz->tts_host, sizeof(g_xz->tts_host), ip);
    g_xz->ui_rev++;
    rt_mutex_release(&g_xz->state_lock);
    LOG_I("Pinned Xiaoyi TTS host -> %s:%u", g_xz->tts_host, (unsigned)g_xz->tts_port);
}

void xiaozhi_speak_text_xiaoyi(const char *text)
{
    if (g_xz == RT_NULL || text == RT_NULL || text[0] == '\0') return;
    char clean[XZ_TTS_TEXT_MAX];
    if (!xz_sanitize_assistant_text(text, clean, sizeof(clean)))
    {
        LOG_I("Skipped non-speakable text: %s", text);
        return;
    }
    xz_set_assistant_text(clean);
    xz_tts_enqueue(clean);
}

static const struct
{
    xz_emotion_t    emo;
    xz_voice_clip_t clip;
    const char     *name;
    const char     *status;
    const char     *line;
} g_gemini_voices[XZ_VOICE_COUNT] = {
    {XZ_EMO_IDLE,      XZ_VOICE_IDLE,      "neutral",   "说话中 · 问候", "主人好，我是Gemini，你的双子星猫娘助手，随时准备为你护航喵。"},
    {XZ_EMO_LISTENING, XZ_VOICE_LISTENING, "listening", "说话中 · 聆听", "嗯，我在认真听呢，主人请说吧喵。"},
    {XZ_EMO_THINKING,  XZ_VOICE_THINKING,  "thinking",  "说话中 · 思考", "稍微想一下，马上整理好答复喵。"},
    {XZ_EMO_SPEAKING,  XZ_VOICE_HAPPY,     "happy",     "说话中 · 开心", "今天状态很不错呢，路上也请安心驾驶喵。"},
    {XZ_EMO_WINKING,   XZ_VOICE_WINKING,   "winking",   "说话中 · 俏皮", "双子星猫娘Gemini随时待命，需要帮忙叫我就好喵。"},
    {XZ_EMO_SURPRISED, XZ_VOICE_ALERT,     "surprised", "说话中 · 警报", "主人小心！检测到闭眼疲劳了，请立刻集中精神或者靠边休息喵！"},
    {XZ_EMO_SAD,       XZ_VOICE_OFFLINE,   "sad",       "说话中 · 离线", "网络好像还没连上呢，去无线网络页面连一下吧喵。"},
    {XZ_EMO_SLEEPY,    XZ_VOICE_SLEEPY,    "sleepy",    "说话中 · 休眠", "先稍微休息一下，有需要随时叫醒我喵。"},
    {XZ_EMO_WINKING,   XZ_VOICE_BIND,      "winking",   "说话中 · 绑定", "请打开小智控制台输入验证码绑定设备，绑好就能语音聊天了喵。"},
};

static xz_state_t g_state_before_local_voice = XZ_STATE_IDLE;
static rt_bool_t  g_greeted_once = RT_FALSE;

void xiaozhi_speak_clip(xz_voice_clip_t clip)
{
    if (g_xz == RT_NULL || clip >= XZ_VOICE_COUNT) return;
    if (g_xz->state == XZ_STATE_LISTENING || xz_speaker_is_enabled())
    {
        return; /* Don't interrupt active live cloud conversation */
    }

    rt_mutex_take(&g_xz->state_lock, RT_WAITING_FOREVER);
    if (g_xz->state != XZ_STATE_SPEAKING)
    {
        g_state_before_local_voice = g_xz->state;
    }
    g_xz->state   = XZ_STATE_SPEAKING;
    g_xz->emotion = g_gemini_voices[clip].emo;
    xz_copy_str(g_xz->emotion_name, sizeof(g_xz->emotion_name), g_gemini_voices[clip].name);
    xz_copy_str(g_xz->status_text,  sizeof(g_xz->status_text),  g_gemini_voices[clip].status);
    xz_copy_str(g_xz->assistant_text, sizeof(g_xz->assistant_text), g_gemini_voices[clip].line);
    g_xz->ui_rev++;
    rt_mutex_release(&g_xz->state_lock);

    xz_audio_play_local_voice(clip);
}

void xz_on_local_voice_finished(void)
{
    if (g_xz == RT_NULL) return;
    rt_mutex_take(&g_xz->state_lock, RT_WAITING_FOREVER);
    if (g_xz->state == XZ_STATE_SPEAKING && !xz_speaker_is_enabled())
    {
        if (g_xz->activation_code[0] != '\0')
        {
            g_xz->state = XZ_STATE_ACTIVATING;
            xz_copy_str(g_xz->status_text, sizeof(g_xz->status_text), "等待绑定 · 点击听她说");
        }
        else if (g_xz->ws_connected)
        {
            g_xz->state = XZ_STATE_IDLE;
            xz_copy_str(g_xz->status_text, sizeof(g_xz->status_text), "待命中 · 点击对话");
        }
        else
        {
            g_xz->state = g_state_before_local_voice;
            xz_copy_str(g_xz->status_text, sizeof(g_xz->status_text),
                        g_xz->wifi_ready ? "休眠中 · 点击唤醒" : "离线语音 · 可点听她说");
        }
        g_xz->ui_rev++;
    }
    rt_mutex_release(&g_xz->state_lock);
}

void xiaozhi_greet_once(void)
{
    if (g_greeted_once || g_xz == RT_NULL) return;
    g_greeted_once = RT_TRUE;
    if (g_xz->state != XZ_STATE_LISTENING && g_xz->state != XZ_STATE_SPEAKING)
    {
        xiaozhi_speak_clip(XZ_VOICE_IDLE);
    }
}

void xiaozhi_cycle_demo_emotion(void)
{
    static unsigned idx = 0U;
    if (g_xz == RT_NULL) return;

    idx = (idx + 1U) % XZ_VOICE_COUNT;
    xiaozhi_speak_clip((xz_voice_clip_t)idx);
}

void xiaozhi_notify_fatigue_alert(float confidence)
{
    if (g_xz == RT_NULL) return;
    /* Preempt TTS speaker immediately so fatigue alarm WAV plays cleanly */
    xiaozhi_audio_preempt_for_alarm();

    char warn[160];
    rt_snprintf(warn, sizeof(warn),
                "主人小心！检测到闭眼疲劳（置信度 %.0f%%），请立刻集中精神或靠边休息喵！",
                (double)(confidence * 100.0f));
    rt_mutex_take(&g_xz->state_lock, RT_WAITING_FOREVER);
    g_xz->emotion = XZ_EMO_SURPRISED;
    xz_copy_str(g_xz->emotion_name, sizeof(g_xz->emotion_name), "surprised");
    xz_copy_str(g_xz->assistant_text, sizeof(g_xz->assistant_text), warn);
    g_xz->ui_rev++;
    rt_mutex_release(&g_xz->state_lock);
}

void xiaozhi_get_snapshot(xz_ui_snapshot_t *out)
{
    if (out == RT_NULL) return;
    rt_memset(out, 0, sizeof(*out));
    if (g_xz == RT_NULL) return;

    rt_mutex_take(&g_xz->state_lock, RT_WAITING_FOREVER);
    out->state         = g_xz->state;
    out->emotion       = g_xz->emotion;
    out->voice_mode    = g_xz->voice_mode;
    out->wifi_ready    = rt_wlan_is_ready();
    out->ws_connected  = g_xz->ws_connected;
    out->multi_turn    = g_xz->multi_turn;
    out->mic_active    = xz_mic_is_enabled() ? RT_TRUE : RT_FALSE;
    out->spk_active    = (xz_speaker_is_enabled() || g_xz->tts_busy || xz_audio_is_local_voice_playing()) ? RT_TRUE : RT_FALSE;
    out->tts_bridge_ok = (g_xz->tts_bridge_ok || xz_tts_net_is_connected()) ? RT_TRUE : RT_FALSE;
    out->mic_level     = xz_audio_get_mic_level();
    out->spk_level     = xz_audio_get_spk_level();
    out->session_turns = g_xz->session_turns;
    out->ui_rev        = g_xz->ui_rev;
    xz_copy_str(out->status_text,     sizeof(out->status_text),     g_xz->status_text);
    xz_copy_str(out->emotion_name,    sizeof(out->emotion_name),    g_xz->emotion_name);
    xz_copy_str(out->user_text,       sizeof(out->user_text),       g_xz->user_text);
    xz_copy_str(out->assistant_text,  sizeof(out->assistant_text),  g_xz->assistant_text);
    xz_copy_str(out->activation_code, sizeof(out->activation_code), g_xz->activation_code);
    xz_copy_str(out->server_host,     sizeof(out->server_host),     g_xz->ws_host);
    rt_mutex_release(&g_xz->state_lock);
}

int xiaozhi_set_server(const char *host, const char *path, const char *token)
{
    if (g_xz == RT_NULL || host == RT_NULL || host[0] == '\0') return -RT_EINVAL;

    rt_mutex_take(&g_xz->state_lock, RT_WAITING_FOREVER);
    xz_copy_str(g_xz->ota_host, sizeof(g_xz->ota_host), host);
    xz_copy_str(g_xz->ws_host,  sizeof(g_xz->ws_host),  host);
    if (path != RT_NULL && path[0] != '\0')
        xz_copy_str(g_xz->ws_path, sizeof(g_xz->ws_path), path);
    if (token != RT_NULL && token[0] != '\0')
        xz_copy_str(g_xz->ws_token, sizeof(g_xz->ws_token), token);
    g_xz->ui_rev++;
    rt_mutex_release(&g_xz->state_lock);

    FILE *fp = fopen(XZ_CFG_PATH, "w");
    if (fp != RT_NULL)
    {
        fprintf(fp, "host=%s\npath=%s\ntoken=%s\nport=%u\ntls=%u\n",
                g_xz->ws_host, g_xz->ws_path, g_xz->ws_token,
                (unsigned)g_xz->ws_port, (unsigned)g_xz->ws_use_tls);
        fclose(fp);
    }

    xiaozhi_reconnect();
    return RT_EOK;
}

/* ---------------- MSH Commands ---------------- */

#ifdef RT_USING_FINSH
#include <finsh.h>

static int cmd_xz_status(void)
{
    xz_ui_snapshot_t s;
    xiaozhi_get_snapshot(&s);
    rt_kprintf("XiaoZhi (Gemini-chan): state=%d emo=%s(%d) wifi=%d ws=%d turns=%lu host=%s\n",
               (int)s.state, s.emotion_name, (int)s.emotion,
               (int)s.wifi_ready, (int)s.ws_connected,
               (unsigned long)s.session_turns, s.server_host);
    rt_kprintf("  MAC=%s ClientID=%s Session=%s\n",
               xz_get_mac_address(), xz_get_client_id(), xz_ws_get_session_id());
    if (s.activation_code[0] != '\0')
        rt_kprintf("  Activation Code (xiaozhi.me): %s\n", s.activation_code);
    rt_kprintf("  Status: %s\n  User: %s\n  Gemini: %s\n",
               s.status_text, s.user_text, s.assistant_text);
    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_xz_status, xz_status, Show XiaoZhi AI & Gemini-chan status);

static int cmd_xz_talk(void)
{
    xiaozhi_trigger_talk();
    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_xz_talk, xz_talk, Trigger/toggle XiaoZhi voice interaction);

static int cmd_xz_stop(void)
{
    xiaozhi_stop_conversation();
    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_xz_stop, xz_stop, Stop active XiaoZhi voice conversation);

static int cmd_xz_reconnect(void)
{
    xiaozhi_reconnect();
    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_xz_reconnect, xz_reconnect, Reconnect XiaoZhi WebSocket);

static int cmd_xz_demo(void)
{
    xiaozhi_cycle_demo_emotion();
    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_xz_demo, xz_demo, Cycle Gemini-chan expression and speak cute TTS);

static int cmd_xz_say(int argc, char **argv)
{
    int idx = (argc >= 2) ? atoi(argv[1]) : 0;
    if (idx < 0 || idx >= (int)XZ_VOICE_COUNT)
    {
        rt_kprintf("Usage: xz_say <0..%d> (0=idle 1=listen 2=think 3=happy 4=wink 5=alert 6=offline 7=sleepy 8=bind)\n",
                   (int)XZ_VOICE_COUNT - 1);
        return -1;
    }
    xiaozhi_speak_clip((xz_voice_clip_t)idx);
    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_xz_say, xz_say, Speak Gemini-chan cute female TTS clip 0..8);

static int cmd_xz_emoji(int argc, char **argv)
{
    if (argc < 2)
    {
        rt_kprintf("Usage: xz_emoji <neutral|listening|thinking|happy|winking|surprised|sad|sleepy>\n");
        return -1;
    }
    xiaozhi_set_emotion_name(argv[1]);
    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_xz_emoji, xz_emoji, Set Gemini-chan expression);

static int cmd_xz_server(int argc, char **argv)
{
    if (argc < 2)
    {
        rt_kprintf("Usage: xz_server <host> [path] [token]\n");
        return -1;
    }
    return xiaozhi_set_server(argv[1], argc >= 3 ? argv[2] : RT_NULL, argc >= 4 ? argv[3] : RT_NULL);
}
MSH_CMD_EXPORT_ALIAS(cmd_xz_server, xz_server, Set custom XiaoZhi server host/path/token);

static int cmd_xz_voice(int argc, char **argv)
{
    if (argc < 2)
    {
        const char *peer = xz_tts_net_peer_ip();
        rt_kprintf("Current voice mode: %s (PC bridge=%s:%u)\n",
                   xiaozhi_get_voice_mode() == XZ_VOICE_MODE_XIAOYI ? "xiaoyi" : "cloud",
                   peer[0] ? peer : ((g_xz && g_xz->tts_host[0]) ? g_xz->tts_host : "waiting-PC"),
                   g_xz ? (unsigned)g_xz->tts_port : XZ_TTS_BRIDGE_PORT);
        rt_kprintf("Usage: xz_voice <xiaoyi|cloud>\n");
        return 0;
    }
    if (strcmp(argv[1], "cloud") == 0)
        xiaozhi_set_voice_mode(XZ_VOICE_MODE_CLOUD);
    else
        xiaozhi_set_voice_mode(XZ_VOICE_MODE_XIAOYI);
    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_xz_voice, xz_voice, Switch voice mode: xiaoyi (cute female) or cloud);

static int cmd_xz_tts_host(int argc, char **argv)
{
    if (argc < 2)
    {
        rt_kprintf("Usage: xz_tts_host <pc_lan_ip>\n");
        return -1;
    }
    xiaozhi_set_tts_host(argv[1]);
    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_xz_tts_host, xz_tts_host, Pin PC LAN IP for XiaoyiNeural live TTS bridge);

static int cmd_xz_tts_test(int argc, char **argv)
{
    const char *txt = (argc >= 2) ? argv[1] : "主人好，我是Gemini，已经切回可爱的萌妹音色了喵。";
    xiaozhi_speak_text_xiaoyi(txt);
    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_xz_tts_test, xz_tts_test, Test live XiaoyiNeural TTS synthesis via LAN bridge);
#endif
