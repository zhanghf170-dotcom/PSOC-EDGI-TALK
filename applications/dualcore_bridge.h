#ifndef DUALCORE_BRIDGE_H__
#define DUALCORE_BRIDGE_H__

#include <rtthread.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Protocol opcodes carried in edge_rc_frame_t.channel[0] */
#define DC_MSG_HEARTBEAT    0xA501U  /* M33 -> M55: [1]=hb_seq, [2]=uptime_s, [3]=heap_kb, [4]=led */
#define DC_MSG_TELEMETRY    0xA502U  /* M55 -> M33: [1]=risk|conf<<8, [2]=danger, [3]=T*10, [4]=H*10, [5]=wifi|fps<<8 */
#define DC_MSG_PING         0xA503U  /* M55 -> M33: [1]=ping_seq, [2]=tick_lo, [3]=tick_hi */
#define DC_MSG_PONG         0xA504U  /* M33 -> M55: [1]=ping_seq, [2]=tick_lo, [3]=tick_hi */
#define DC_MSG_CMD          0xA505U  /* M55 -> M33: [1]=danger_active */

#define DC_ONLINE_TIMEOUT_MS 1500U

typedef struct
{
    rt_bool_t ipc_ready;
    rt_bool_t m33_online;
    uint32_t  m33_hb_seq;
    uint32_t  m33_uptime_s;
    uint32_t  m33_free_heap_kb;
    uint32_t  tx_count;
    uint32_t  rx_count;
    uint32_t  last_rtt_ms;
    rt_tick_t last_rx_tick;
} dualcore_status_t;

int  dualcore_bridge_init(void);
void dualcore_get_status(dualcore_status_t *out);
void dualcore_get_brief(rt_bool_t *online, uint32_t *hb_seq, uint32_t *rtt_ms);
int  dualcore_send_ping(void);
void dualcore_notify_danger(rt_bool_t active);

#ifdef __cplusplus
}
#endif

#endif /* DUALCORE_BRIDGE_H__ */
