/*
 * Dual-Core IPC1 Bridge (Cortex-M55 <-> Cortex-M33)
 *
 * M55 receives M33 frames on EDGE_IPC0_DEVICE_NAME ("ipc1") and transmits on
 * EDGE_IPC1_DEVICE_NAME ("ipc0"), matching official Edgi_Talk_M55_IPC mapping.
 * Falls back cleanly to "M55 SOLO" if CM33 companion firmware is absent/sleeping.
 */

#include <rtthread.h>
#include <rtdevice.h>
#include <string.h>
#include <finsh.h>
#include <msh.h>

#include "drv_ipc.h"
#include "dualcore_bridge.h"
#include "uvc_ai_app.h"
#include "wifi_stream.h"

#define DBG_TAG "dc.bridge"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

extern float g_aht_temp;
extern float g_aht_humi;

static struct
{
    rt_bool_t   init;
    rt_bool_t   ipc_ready;
    rt_device_t rx_dev;
    rt_device_t tx_dev;

    uint32_t  m33_hb_seq;
    uint32_t  m33_uptime_s;
    uint32_t  m33_free_heap_kb;
    uint32_t  tx_count;
    uint32_t  rx_count;
    uint32_t  ping_seq;
    uint32_t  last_rtt_ms;
    rt_tick_t last_rx_tick;

    volatile rt_bool_t ping_req;
    volatile rt_bool_t cmd_req;
    volatile rt_bool_t cmd_danger_active;
} g_dc;

static void dc_send_frame(const edge_rc_frame_t *frame)
{
    if (!g_dc.ipc_ready || g_dc.tx_dev == RT_NULL || frame == RT_NULL)
    {
        return;
    }
    if (rt_device_write(g_dc.tx_dev, 0, frame, 1) == 1)
    {
        g_dc.tx_count++;
    }
}

static void dc_send_telemetry_now(void)
{
    edge_rc_frame_t tx;
    uvc_ai_telemetry_t tel;
    wifi_status_t wst;

    uvc_ai_app_get_telemetry(&tel);
    wifi_mgr_get_status(&wst);

    uint8_t conf_pct = (uint8_t)(tel.max_closed_conf * 100.0f);
    if (conf_pct > 100U) conf_pct = 100U;
    int16_t t_x10 = (int16_t)(g_aht_temp * 10.0f);
    int16_t h_x10 = (int16_t)(g_aht_humi * 10.0f);

    rt_memset(&tx, 0, sizeof(tx));
    tx.seq = g_dc.tx_count + 1U;
    tx.channel[0] = DC_MSG_TELEMETRY;
    tx.channel[1] = (uint16_t)(((uint16_t)conf_pct << 8) | (tel.risk_level & 0xFFU));
    tx.channel[2] = (uint16_t)(tel.danger_count & 0xFFFFU);
    tx.channel[3] = (uint16_t)t_x10;
    tx.channel[4] = (uint16_t)h_x10;
    tx.channel[5] = (uint16_t)(((uint16_t)(wst.stream_fps & 0xFFU) << 8) | (wst.ready ? 1U : 0U));
    dc_send_frame(&tx);
}

static void dc_send_ping_now(void)
{
    edge_rc_frame_t tx;
    uint32_t tick_ms = (uint32_t)(rt_tick_get() * (1000U / RT_TICK_PER_SECOND));

    rt_memset(&tx, 0, sizeof(tx));
    tx.seq = ++g_dc.ping_seq;
    tx.channel[0] = DC_MSG_PING;
    tx.channel[1] = (uint16_t)(g_dc.ping_seq & 0xFFFFU);
    tx.channel[2] = (uint16_t)(tick_ms & 0xFFFFU);
    tx.channel[3] = (uint16_t)(tick_ms >> 16);
    dc_send_frame(&tx);
}

static void dc_handle_rx_frame(const edge_rc_frame_t *rx)
{
    if ((rx->magic != RC_MAGIC_WORD) || (rx->role != RC_ROLE_M33))
    {
        return;
    }

    g_dc.rx_count++;
    g_dc.last_rx_tick = rt_tick_get();

    switch (rx->channel[0])
    {
    case DC_MSG_HEARTBEAT:
        g_dc.m33_hb_seq      = ((uint32_t)rx->channel[1] == 0U && rx->seq > 0U)
                               ? rx->seq
                               : ((uint32_t)rx->channel[1]);
        if (rx->seq > g_dc.m33_hb_seq) g_dc.m33_hb_seq = rx->seq;
        g_dc.m33_uptime_s     = rx->channel[2];
        g_dc.m33_free_heap_kb = rx->channel[3];
        break;

    case DC_MSG_PONG:
    {
        uint32_t sent_ms = (uint32_t)rx->channel[2] | ((uint32_t)rx->channel[3] << 16);
        uint32_t now_ms  = (uint32_t)(rt_tick_get() * (1000U / RT_TICK_PER_SECOND));
        g_dc.last_rtt_ms = (now_ms >= sent_ms) ? (now_ms - sent_ms) : 0U;
        break;
    }

    default:
        break;
    }
}

static void dc_thread_entry(void *parameter)
{
    rt_tick_t last_tx_tick = 0U;
    rt_tick_t last_probe_tick = 0U;
    (void)parameter;

    if (edge_ipc_device_register() == RT_EOK)
    {
        g_dc.rx_dev = edge_ipc_device_find(EDGE_IPC0_DEVICE_NAME); /* "ipc1" */
        g_dc.tx_dev = edge_ipc_device_find(EDGE_IPC1_DEVICE_NAME); /* "ipc0" */

        if (g_dc.rx_dev != RT_NULL && g_dc.tx_dev != RT_NULL &&
            rt_device_open(g_dc.rx_dev, RT_DEVICE_OFLAG_RDWR) == RT_EOK &&
            rt_device_open(g_dc.tx_dev, RT_DEVICE_OFLAG_RDWR) == RT_EOK)
        {
            g_dc.ipc_ready = RT_TRUE;
            LOG_I("IPC1 ready (rx=%s tx=%s)", EDGE_IPC0_DEVICE_NAME, EDGE_IPC1_DEVICE_NAME);
        }
        else
        {
            LOG_W("IPC1 open unavailable; running M55 SOLO");
        }
    }

    while (1)
    {
        edge_rc_frame_t rx;
        rt_tick_t now = rt_tick_get();

        if (g_dc.ipc_ready)
        {
            while (rt_device_read(g_dc.rx_dev, 0, &rx, 1) == 1)
            {
                dc_handle_rx_frame(&rx);
            }

            if (g_dc.ping_req)
            {
                g_dc.ping_req = RT_FALSE;
                dc_send_ping_now();
            }

            if (g_dc.cmd_req)
            {
                edge_rc_frame_t cmd;
                g_dc.cmd_req = RT_FALSE;
                rt_memset(&cmd, 0, sizeof(cmd));
                cmd.seq = g_dc.tx_count + 1U;
                cmd.channel[0] = DC_MSG_CMD;
                cmd.channel[1] = g_dc.cmd_danger_active ? 1U : 0U;
                dc_send_frame(&cmd);
            }

            rt_bool_t m33_up = (g_dc.last_rx_tick != 0U) &&
                               (((uint32_t)(now - g_dc.last_rx_tick) * (1000U / RT_TICK_PER_SECOND)) < DC_ONLINE_TIMEOUT_MS);

            if (m33_up)
            {
                if ((now - last_tx_tick) >= rt_tick_from_millisecond(500))
                {
                    dc_send_telemetry_now();
                    last_tx_tick = now;
                }
            }
            else if ((now - last_probe_tick) >= rt_tick_from_millisecond(3000))
            {
                /* Gentle 3s ping when M33 appears asleep/absent (avoids busy-retry in SOLO mode) */
                last_probe_tick = now;
                if (g_dc.tx_count == 0U || g_dc.rx_count > 0U)
                {
                    dc_send_ping_now();
                }
            }
        }

        rt_thread_mdelay(25);
    }
}

int dualcore_bridge_init(void)
{
    rt_thread_t t;
    if (g_dc.init) return RT_EOK;

    rt_memset(&g_dc, 0, sizeof(g_dc));
    t = rt_thread_create("dc_ipc", dc_thread_entry, RT_NULL, 2048, 18, 10);
    if (t == RT_NULL) return -RT_ENOMEM;

    g_dc.init = RT_TRUE;
    rt_thread_startup(t);
    return RT_EOK;
}

void dualcore_get_status(dualcore_status_t *out)
{
    rt_tick_t now = rt_tick_get();
    if (out == NULL) return;

    rt_memset(out, 0, sizeof(*out));
    out->ipc_ready        = g_dc.ipc_ready;
    out->m33_hb_seq       = g_dc.m33_hb_seq;
    out->m33_uptime_s     = g_dc.m33_uptime_s;
    out->m33_free_heap_kb = g_dc.m33_free_heap_kb;
    out->tx_count         = g_dc.tx_count;
    out->rx_count         = g_dc.rx_count;
    out->last_rtt_ms      = g_dc.last_rtt_ms;
    out->last_rx_tick     = g_dc.last_rx_tick;

    if (g_dc.last_rx_tick != 0U)
    {
        uint32_t dt = ((uint32_t)(now - g_dc.last_rx_tick)) * (1000U / RT_TICK_PER_SECOND);
        out->m33_online = (dt < DC_ONLINE_TIMEOUT_MS) ? RT_TRUE : RT_FALSE;
    }
}

void dualcore_get_brief(rt_bool_t *online, uint32_t *hb_seq, uint32_t *rtt_ms)
{
    dualcore_status_t st;
    dualcore_get_status(&st);
    if (online) *online = st.m33_online;
    if (hb_seq) *hb_seq = st.m33_hb_seq;
    if (rtt_ms) *rtt_ms = st.last_rtt_ms;
}

int dualcore_send_ping(void)
{
    if (!g_dc.init || !g_dc.ipc_ready) return -RT_ERROR;
    g_dc.ping_req = RT_TRUE;
    return RT_EOK;
}

void dualcore_notify_danger(rt_bool_t active)
{
    if (!g_dc.init || !g_dc.ipc_ready) return;
    g_dc.cmd_danger_active = active;
    g_dc.cmd_req = RT_TRUE;
}

static int cmd_ipc_ping(int argc, char **argv)
{
    dualcore_status_t st;
    (void)argc;
    (void)argv;

    if (dualcore_send_ping() != RT_EOK)
    {
        rt_kprintf("IPC not ready\n");
        return -1;
    }
    rt_thread_mdelay(80);
    dualcore_get_status(&st);
    rt_kprintf("IPC1 M33=%s HB=#%lu up=%lus rtt=%lums tx=%lu rx=%lu\n",
               st.m33_online ? "ONLINE" : "OFFLINE (M55 SOLO)",
               (unsigned long)st.m33_hb_seq,
               (unsigned long)st.m33_uptime_s,
               (unsigned long)st.last_rtt_ms,
               (unsigned long)st.tx_count,
               (unsigned long)st.rx_count);
    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_ipc_ping, ipc_ping, Ping Cortex-M33 over IPC1);
