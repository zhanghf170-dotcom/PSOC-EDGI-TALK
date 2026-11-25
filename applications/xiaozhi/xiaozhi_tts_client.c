/*
 * LAN XiaoyiNeural Live TTS Bridge Client (Firewall-Free Reverse TCP + UDP Discovery)
 *
 * Kept in its own C translation unit so RT-Thread SAL headers (<sys/socket.h>,
 * <arpa/inet.h>) never collide with raw lwIP headers used by websocket_client.h
 * in xiaozhi.cpp.
 *
 * Architecture (zero Windows Firewall setup required!):
 *   1. Board listens on UDP :8890 and replies "GEMTTS_BOARD:8890" to PC discovery probes.
 *   2. Board listens on TCP :8890; PC script connects OUTBOUND to <board_ip>:8890
 *      (Windows Firewall always permits outbound TCP & stateful UDP replies).
 *   3. When XiaoZhi AI produces reply text, board sends "<utf8 text>\n" over the
 *      open TCP link and streams back framed 24 kHz 60 ms Opus packets to ES8388.
 *   4. Optional fallback: if user explicitly pinned a host via `xz_tts_host <ip>`
 *      and no reverse PC client is connected, board attempts direct outbound TCP.
 */

#include <rtthread.h>
#include <wlan_mgnt.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <string.h>
#include <stdio.h>
#include "wifi_stream.h"
#include "xiaozhi.h"

#define DBG_TAG "xz.tts"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

static uint16_t          g_listen_port   = 8890U;
static int               g_bridge_sock   = -1;
static volatile rt_bool_t g_streaming    = RT_FALSE;
static char              g_peer_ip[24]   = {0};
static rt_mutex_t        g_sock_lock     = RT_NULL;
static rt_bool_t         g_srv_inited    = RT_FALSE;

static void tts_close_bridge_locked(void)
{
    if (g_bridge_sock >= 0)
    {
        closesocket(g_bridge_sock);
        g_bridge_sock = -1;
    }
    g_peer_ip[0] = '\0';
}

rt_bool_t xz_tts_net_is_connected(void)
{
    return (g_bridge_sock >= 0) ? RT_TRUE : RT_FALSE;
}

const char *xz_tts_net_peer_ip(void)
{
    return (g_bridge_sock >= 0 && g_peer_ip[0] != '\0') ? g_peer_ip : "";
}

static void xz_tts_srv_entry(void *param)
{
    (void)param;
    int tcp_srv = -1;
    int udp_srv = -1;

    while (1)
    {
        if (!rt_wlan_is_ready())
        {
            if (tcp_srv >= 0) { closesocket(tcp_srv); tcp_srv = -1; }
            if (udp_srv >= 0) { closesocket(udp_srv); udp_srv = -1; }
            if (g_sock_lock != RT_NULL)
            {
                rt_mutex_take(g_sock_lock, RT_WAITING_FOREVER);
                tts_close_bridge_locked();
                rt_mutex_release(g_sock_lock);
            }
            rt_thread_mdelay(1000);
            continue;
        }

        if (tcp_srv < 0)
        {
            tcp_srv = socket(AF_INET, SOCK_STREAM, 0);
            if (tcp_srv >= 0)
            {
                int opt = 1;
                setsockopt(tcp_srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
                struct sockaddr_in addr;
                rt_memset(&addr, 0, sizeof(addr));
                addr.sin_family      = AF_INET;
                addr.sin_port        = htons(g_listen_port);
                addr.sin_addr.s_addr = INADDR_ANY;
                if (bind(tcp_srv, (struct sockaddr *)&addr, sizeof(addr)) == 0 &&
                    listen(tcp_srv, 2) == 0)
                {
                    LOG_I("Xiaoyi TTS TCP listener ready on :%u", (unsigned)g_listen_port);
                }
                else
                {
                    closesocket(tcp_srv);
                    tcp_srv = -1;
                }
            }
        }

        if (udp_srv < 0)
        {
            udp_srv = socket(AF_INET, SOCK_DGRAM, 0);
            if (udp_srv >= 0)
            {
                int opt = 1;
                setsockopt(udp_srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
                struct sockaddr_in uaddr;
                rt_memset(&uaddr, 0, sizeof(uaddr));
                uaddr.sin_family      = AF_INET;
                uaddr.sin_port        = htons(g_listen_port);
                uaddr.sin_addr.s_addr = INADDR_ANY;
                if (bind(udp_srv, (struct sockaddr *)&uaddr, sizeof(uaddr)) != 0)
                {
                    closesocket(udp_srv);
                    udp_srv = -1;
                }
            }
        }

        if (tcp_srv < 0 && udp_srv < 0)
        {
            rt_thread_mdelay(1000);
            continue;
        }

        fd_set rfds;
        FD_ZERO(&rfds);
        int maxfd = -1;
        if (tcp_srv >= 0) { FD_SET(tcp_srv, &rfds); if (tcp_srv > maxfd) maxfd = tcp_srv; }
        if (udp_srv >= 0) { FD_SET(udp_srv, &rfds); if (udp_srv > maxfd) maxfd = udp_srv; }

        int idle_peer = -1;
        if (!g_streaming && g_bridge_sock >= 0)
        {
            idle_peer = g_bridge_sock;
            FD_SET(idle_peer, &rfds);
            if (idle_peer > maxfd) maxfd = idle_peer;
        }

        struct timeval tv = { .tv_sec = 0, .tv_usec = 500000 };
        int ret = select(maxfd + 1, &rfds, RT_NULL, RT_NULL, &tv);
        if (ret <= 0) continue;

        /* 1. Reply to PC UDP discovery probe */
        if (udp_srv >= 0 && FD_ISSET(udp_srv, &rfds))
        {
            char buf[64];
            struct sockaddr_in from;
            socklen_t flen = sizeof(from);
            int n = recvfrom(udp_srv, buf, sizeof(buf) - 1U, 0, (struct sockaddr *)&from, &flen);
            if (n > 0)
            {
                buf[n] = '\0';
                if (strncmp(buf, "GEMTTS_PC_HELLO", 15) == 0)
                {
                    char rep[48];
                    int rlen = rt_snprintf(rep, sizeof(rep), "GEMTTS_BOARD:%u", (unsigned)g_listen_port);
                    (void)sendto(udp_srv, rep, (size_t)rlen, 0, (struct sockaddr *)&from, flen);
                }
            }
        }

        /* 2. Accept incoming PC TTS bridge connection */
        if (tcp_srv >= 0 && FD_ISSET(tcp_srv, &rfds))
        {
            struct sockaddr_in caddr;
            socklen_t clen = sizeof(caddr);
            int cfd = accept(tcp_srv, (struct sockaddr *)&caddr, &clen);
            if (cfd >= 0)
            {
                int nd = 1;
                setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &nd, sizeof(nd));
                struct timeval tv_rx = { .tv_sec = 10, .tv_usec = 0 };
                setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv_rx, sizeof(tv_rx));
                struct timeval tv_tx = { .tv_sec = 2, .tv_usec = 0 };
                setsockopt(cfd, SOL_SOCKET, SO_SNDTIMEO, &tv_tx, sizeof(tv_tx));

                rt_mutex_take(g_sock_lock, RT_WAITING_FOREVER);
                tts_close_bridge_locked();
                g_bridge_sock = cfd;
                (void)send(cfd, "GEMTTS_READY\n", 13, 0);
                const char *ip = inet_ntoa(caddr.sin_addr);
                if (ip != RT_NULL)
                {
                    rt_strncpy(g_peer_ip, ip, sizeof(g_peer_ip) - 1U);
                    g_peer_ip[sizeof(g_peer_ip) - 1U] = '\0';
                }
                rt_mutex_release(g_sock_lock);
                LOG_I("PC Xiaoyi TTS bridge connected from %s", g_peer_ip);
            }
        }

        /* 3. Detect idle PC disconnect cleanly */
        if (idle_peer >= 0 && !g_streaming && FD_ISSET(idle_peer, &rfds))
        {
            if (rt_mutex_take(g_sock_lock, RT_WAITING_NO) == RT_EOK)
            {
                if (!g_streaming && g_bridge_sock == idle_peer)
                {
                    uint8_t b = 0;
                    int n = recv(idle_peer, &b, 1, 0);
                    if (n <= 0)
                    {
                        LOG_I("PC Xiaoyi TTS bridge disconnected");
                        tts_close_bridge_locked();
                    }
                }
                rt_mutex_release(g_sock_lock);
            }
        }
    }
}

void xz_tts_net_init(uint16_t port)
{
    if (g_srv_inited) return;
    if (port != 0U) g_listen_port = port;
    g_sock_lock = rt_mutex_create("xz_tslk", RT_IPC_FLAG_PRIO);
    rt_thread_t tid = rt_thread_create("xz_tsrv", xz_tts_srv_entry, RT_NULL, 4096, 16, 10);
    if (tid != RT_NULL)
    {
        rt_thread_startup(tid);
        g_srv_inited = RT_TRUE;
    }
}

static int tts_recv_exact(int sock, uint8_t *buf, size_t len, volatile rt_bool_t *abort_flag)
{
    size_t got = 0U;
    while (got < len)
    {
        if (abort_flag != RT_NULL && *abort_flag) return -1;
        int n = recv(sock, buf + got, len - got, 0);
        if (n <= 0) return -1;
        got += (size_t)n;
    }
    return 0;
}

static rt_bool_t tts_exchange_on_socket(int sock, const char *text,
                                        volatile rt_bool_t *abort_flag,
                                        rt_bool_t *proto_broken_out)
{
    size_t tlen = strlen(text);
    if (send(sock, text, tlen, 0) <= 0 || send(sock, "\n", 1, 0) <= 0)
    {
        if (proto_broken_out != RT_NULL) *proto_broken_out = RT_TRUE;
        return RT_FALSE;
    }

    rt_bool_t started = RT_FALSE;
    rt_bool_t clean_eos = RT_FALSE;
    uint8_t pkt_buf[512];

    while (abort_flag == RT_NULL || !(*abort_flag))
    {
        uint8_t hdr[2];
        if (tts_recv_exact(sock, hdr, 2U, abort_flag) != 0) break;
        uint16_t plen = (uint16_t)hdr[0] | ((uint16_t)hdr[1] << 8);
        if (plen == 0U)
        {
            clean_eos = RT_TRUE;
            break;
        }
        if (plen > sizeof(pkt_buf)) break;
        if (tts_recv_exact(sock, pkt_buf, plen, abort_flag) != 0) break;

        if (!started)
        {
            if (xz_audio_stream_begin() != RT_EOK) break;
            started = RT_TRUE;
        }
        xz_audio_stream_feed_opus(pkt_buf, plen);
    }

    if (started)
    {
        xz_audio_stream_end();
    }
    if (proto_broken_out != RT_NULL)
    {
        *proto_broken_out = clean_eos ? RT_FALSE : RT_TRUE;
    }
    return started;
}

rt_bool_t xz_tts_net_stream_sentence(char *host_buf, size_t host_cap, uint16_t port,
                                     const char *text, volatile rt_bool_t *abort_flag,
                                     rt_bool_t *bridge_ok_out)
{
    (void)host_cap;
    if (!rt_wlan_is_ready() || text == RT_NULL || text[0] == '\0')
        return RT_FALSE;

    /* Primary path: persistent reverse TCP link opened by PC (zero firewall issues!) */
    if (g_sock_lock != RT_NULL)
    {
        rt_mutex_take(g_sock_lock, RT_WAITING_FOREVER);
        if (g_bridge_sock >= 0)
        {
            g_streaming = RT_TRUE;
            rt_bool_t broken = RT_FALSE;
            rt_bool_t ok = tts_exchange_on_socket(g_bridge_sock, text, abort_flag, &broken);
            if (broken)
            {
                /* Close on abort/timeout so PC reconnects cleanly with zero stale bytes */
                tts_close_bridge_locked();
            }
            g_streaming = RT_FALSE;
            rt_bool_t still_up = (g_bridge_sock >= 0) ? RT_TRUE : RT_FALSE;
            rt_mutex_release(g_sock_lock);

            if (bridge_ok_out != RT_NULL) *bridge_ok_out = still_up;
            return ok;
        }
        rt_mutex_release(g_sock_lock);
    }

    /* Optional fallback: direct outbound TCP ONLY if user pinned host_buf via xz_tts_host */
    if (host_buf != RT_NULL && host_buf[0] != '\0')
    {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) return RT_FALSE;

        struct timeval tv_conn = { .tv_sec = 2, .tv_usec = 0 };
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv_conn, sizeof(tv_conn));
        struct timeval tv_rx = { .tv_sec = 8, .tv_usec = 0 };
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv_rx, sizeof(tv_rx));

        struct sockaddr_in addr;
        rt_memset(&addr, 0, sizeof(addr));
        addr.sin_family      = AF_INET;
        addr.sin_port        = htons(port);
        addr.sin_addr.s_addr = inet_addr(host_buf);

        if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) == 0)
        {
            if (bridge_ok_out != RT_NULL) *bridge_ok_out = RT_TRUE;
            rt_bool_t ok = tts_exchange_on_socket(sock, text, abort_flag, RT_NULL);
            closesocket(sock);
            return ok;
        }
        closesocket(sock);
    }

    if (bridge_ok_out != RT_NULL) *bridge_ok_out = RT_FALSE;
    return RT_FALSE;
}
