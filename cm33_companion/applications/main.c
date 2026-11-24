/*
 * Cortex-M33 Companion Firmware — IPC1 Responder & Safety Supervisor
 * Console: UART5 (never conflicts with CM55 UART2)
 * Status LED: Blue LED P16[5] (fast flash when CM55 flags danger, calm heartbeat otherwise)
 */

#include <rtthread.h>
#include <rtdevice.h>
#include <board.h>
#include <string.h>
#include <finsh.h>
#include <msh.h>

#include "drv_ipc.h"

#define LED_PIN_B            GET_PIN(16, 5)
#define HB_INTERVAL_MS       500U

#define DC_MSG_HEARTBEAT     0xA501U
#define DC_MSG_TELEMETRY     0xA502U
#define DC_MSG_PING          0xA503U
#define DC_MSG_PONG          0xA504U
#define DC_MSG_CMD           0xA505U

static struct
{
    rt_device_t tx_dev;
    rt_device_t rx_dev;
    rt_bool_t   ipc_ready;
    uint32_t    hb_seq;
    uint32_t    rx_count;
    uint32_t    tx_count;
    uint8_t     m55_risk;
    uint8_t     m55_conf_pct;
    uint16_t    m55_danger_cnt;
    int16_t     m55_temp_x10;
    int16_t     m55_humi_x10;
    uint8_t     m55_wifi_ready;
    uint8_t     m55_stream_fps;
    rt_bool_t   m55_danger_active;
} g_m33;

static uint32_t m33_get_free_heap_kb(void)
{
#ifdef RT_USING_MEMHEAP_AS_HEAP
    return 0U;
#else
    rt_size_t total = 0, used = 0, max_used = 0;
    rt_memory_info(&total, &used, &max_used);
    return (total >= used) ? (uint32_t)((total - used) / 1024U) : 0U;
#endif
}

static void m33_send_pong(const edge_rc_frame_t *ping)
{
    edge_rc_frame_t tx;
    if (!g_m33.ipc_ready) return;

    rt_memset(&tx, 0, sizeof(tx));
    tx.seq = ping->seq;
    tx.channel[0] = DC_MSG_PONG;
    tx.channel[1] = ping->channel[1];
    tx.channel[2] = ping->channel[2];
    tx.channel[3] = ping->channel[3];
    if (rt_device_write(g_m33.tx_dev, 0, &tx, 1) == 1)
    {
        g_m33.tx_count++;
    }
}

static void m33_send_heartbeat(uint8_t led_state)
{
    edge_rc_frame_t tx;
    uint32_t up_s = (uint32_t)(rt_tick_get() / RT_TICK_PER_SECOND);
    if (!g_m33.ipc_ready) return;

    rt_memset(&tx, 0, sizeof(tx));
    tx.seq = ++g_m33.hb_seq;
    tx.channel[0] = DC_MSG_HEARTBEAT;
    tx.channel[1] = (uint16_t)(g_m33.hb_seq & 0xFFFFU);
    tx.channel[2] = (uint16_t)(up_s & 0xFFFFU);
    tx.channel[3] = (uint16_t)(m33_get_free_heap_kb() & 0xFFFFU);
    tx.channel[4] = led_state;
    if (rt_device_write(g_m33.tx_dev, 0, &tx, 1) == 1)
    {
        g_m33.tx_count++;
    }
}

static void m33_ipc_thread_entry(void *parameter)
{
    rt_tick_t last_hb = 0U;
    (void)parameter;

    if (edge_ipc_device_register() == RT_EOK)
    {
        /* M33 transmits on EDGE_IPC0_DEVICE_NAME ("ipc1") and receives on EDGE_IPC1_DEVICE_NAME ("ipc0") */
        g_m33.tx_dev = edge_ipc_device_find(EDGE_IPC0_DEVICE_NAME);
        g_m33.rx_dev = edge_ipc_device_find(EDGE_IPC1_DEVICE_NAME);

        if (g_m33.tx_dev != RT_NULL && g_m33.rx_dev != RT_NULL &&
            rt_device_open(g_m33.tx_dev, RT_DEVICE_OFLAG_RDWR) == RT_EOK &&
            rt_device_open(g_m33.rx_dev, RT_DEVICE_OFLAG_RDWR) == RT_EOK)
        {
            g_m33.ipc_ready = RT_TRUE;
            rt_kprintf("[M33] IPC1 companion link up (tx=%s rx=%s)\n",
                       EDGE_IPC0_DEVICE_NAME, EDGE_IPC1_DEVICE_NAME);
        }
    }

    while (1)
    {
        edge_rc_frame_t rx;
        rt_tick_t now = rt_tick_get();

        if (g_m33.ipc_ready)
        {
            while (rt_device_read(g_m33.rx_dev, 0, &rx, 1) == 1)
            {
                if (rx.magic == RC_MAGIC_WORD && rx.role == RC_ROLE_M55_ECHO)
                {
                    g_m33.rx_count++;
                    if (rx.channel[0] == DC_MSG_PING)
                    {
                        m33_send_pong(&rx);
                    }
                    else if (rx.channel[0] == DC_MSG_TELEMETRY)
                    {
                        g_m33.m55_risk       = (uint8_t)(rx.channel[1] & 0xFFU);
                        g_m33.m55_conf_pct   = (uint8_t)(rx.channel[1] >> 8);
                        g_m33.m55_danger_cnt = rx.channel[2];
                        g_m33.m55_temp_x10   = (int16_t)rx.channel[3];
                        g_m33.m55_humi_x10   = (int16_t)rx.channel[4];
                        g_m33.m55_wifi_ready = (uint8_t)(rx.channel[5] & 0xFFU);
                        g_m33.m55_stream_fps = (uint8_t)(rx.channel[5] >> 8);
                    }
                    else if (rx.channel[0] == DC_MSG_CMD)
                    {
                        g_m33.m55_danger_active = (rx.channel[1] != 0U) ? RT_TRUE : RT_FALSE;
                    }
                }
            }

            if ((now - last_hb) >= rt_tick_from_millisecond(HB_INTERVAL_MS))
            {
                m33_send_heartbeat(g_m33.m55_danger_active ? 1U : 0U);
                last_hb = now;
            }
        }

        rt_thread_mdelay(20);
    }
}

static int cmd_m33_status(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    rt_kprintf("[M33] ipc=%s hb=#%lu tx=%lu rx=%lu | M55 risk=%u (%u%%) danger=%u T=%.1fC H=%.1f%% wifi=%u (%ufps)\n",
               g_m33.ipc_ready ? "READY" : "OFF",
               (unsigned long)g_m33.hb_seq,
               (unsigned long)g_m33.tx_count,
               (unsigned long)g_m33.rx_count,
               (unsigned)g_m33.m55_risk,
               (unsigned)g_m33.m55_conf_pct,
               (unsigned)g_m33.m55_danger_cnt,
               (double)(g_m33.m55_temp_x10 / 10.0f),
               (double)(g_m33.m55_humi_x10 / 10.0f),
               (unsigned)g_m33.m55_wifi_ready,
               (unsigned)g_m33.m55_stream_fps);
    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_m33_status, m33_status, Print Cortex-M33 companion and IPC status);

int main(void)
{
    rt_kprintf("\n========================================\n");
    rt_kprintf("  Edgi-Talk CM33 Companion Supervisor\n");
    rt_kprintf("  Console: UART5 | Core: CM33 @ 200MHz\n");
    rt_kprintf("========================================\n");

    rt_thread_t t = rt_thread_create("m33_ipc", m33_ipc_thread_entry, RT_NULL,
                                     2048, 12, 10);
    if (t != RT_NULL)
    {
        rt_thread_startup(t);
    }

    rt_pin_mode(LED_PIN_B, PIN_MODE_OUTPUT);
    while (1)
    {
        uint32_t period = g_m33.m55_danger_active ? 150U : 500U;
        rt_pin_write(LED_PIN_B, PIN_LOW);
        rt_thread_mdelay(period);
        rt_pin_write(LED_PIN_B, PIN_HIGH);
        rt_thread_mdelay(period);
    }

    return 0;
}
