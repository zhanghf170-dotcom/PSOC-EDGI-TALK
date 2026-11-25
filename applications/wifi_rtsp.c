#include "wifi_rtsp.h"
#include "wifi_stream.h"

#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define DBG_TAG "wifi.rtsp"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

#define RTSP_PUBLIC "Public: OPTIONS, DESCRIBE, SETUP, PLAY, PAUSE, GET_PARAMETER, TEARDOWN\r\n"
#define RTSP_PATH "/camera"
#define RTSP_CONTROL "/track1"
#define RTSP_SESSION_SECONDS 30U
#define RTP_JPEG_PAYLOAD_TYPE 26U
#define RTP_JPEG_FRAGMENT 1200U

struct rtsp_session
{
    int fd;
    uint32_t id;
    uint32_t jpeg_seq;
    uint32_t packets_sent;
    uint32_t octets_sent;
    uint32_t last_rtcp;
    uint16_t seq;
    uint8_t channel;
    rt_bool_t setup;
    rt_bool_t playing;
    char url[64];
};

struct rtp_jpeg
{
    const uint8_t *scan;
    size_t scan_len;
    const uint8_t *quant[2];
    uint8_t width;
    uint8_t height;
    uint8_t type;
};

static volatile rt_bool_t g_rtsp_enabled = RT_TRUE;
static volatile rt_bool_t g_rtsp_playing;
static uint32_t g_rtsp_frames;
static struct rt_thread g_rtsp_thread;
rt_align(RT_ALIGN_SIZE) static uint8_t g_rtsp_stack[6144];

rt_bool_t wifi_rtsp_is_playing(void)
{
    return g_rtsp_enabled && g_rtsp_playing;
}

static int rtsp_send(int fd, const void *data, size_t length)
{
    const uint8_t *p = data;
    while (length != 0U)
    {
        int n = send(fd, p, length, 0);
        if (n <= 0)
            return -1;
        p += n;
        length -= (size_t)n;
    }
    return 0;
}

static unsigned int read_be16(const uint8_t *src)
{
    return ((unsigned int)src[0] << 8) | src[1];
}

static void write_be16(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)(value >> 8);
    dst[1] = (uint8_t)value;
}

static void write_be32(uint8_t *dst, uint32_t value)
{
    write_be16(dst, (uint16_t)(value >> 16));
    write_be16(dst + 2, (uint16_t)value);
}

/* RFC 2435 carries the scan and 8-bit quantization tables for baseline JPEG. */
static rt_bool_t rtsp_parse_jpeg(const uint8_t *jpeg, size_t size, struct rtp_jpeg *frame)
{
    static const uint8_t scan_header[] = { 3, 1, 0, 2, 0x11, 3, 0x11, 0, 63, 0 };
    if (size < 4U || read_be16(jpeg) != 0xffd8U ||
        read_be16(jpeg + size - 2U) != 0xffd9U)
        return RT_FALSE;

    for (size_t pos = 2U; pos + 4U <= size;)
    {
        if (jpeg[pos++] != 0xffU)
            break;
        uint8_t marker = jpeg[pos++];
        unsigned int length = read_be16(jpeg + pos);
        if (length < 2U || length > size - pos)
            break;
        const uint8_t *body = jpeg + pos + 2U;
        pos += length;

        if (marker == 0xdbU)
        {
            if ((length - 2U) % 65U != 0U)
                break;
            for (unsigned int i = 0; i < length - 2U; i += 65U)
            {
                if (body[i] > 1U)
                    return RT_FALSE;
                frame->quant[body[i]] = body + i + 1U;
            }
        }
        else if (marker == 0xc0U && length == 17U && body[0] == 8U && body[5] == 3U)
        {
            unsigned int w = read_be16(body + 3U);
            unsigned int h = read_be16(body + 1U);
            static const uint8_t chroma[] = { 2, 0x11, 1, 3, 0x11, 1 };
            if (w == 0U || h == 0U || w > 2040U || h > 2040U || w % 8U || h % 8U ||
                body[6] != 1U || (body[7] != 0x21U && body[7] != 0x22U) || body[8] != 0U ||
                memcmp(body + 9U, chroma, sizeof(chroma)) != 0)
                break;
            frame->width = (uint8_t)(w / 8U);
            frame->height = (uint8_t)(h / 8U);
            frame->type = body[7] == 0x22U ? 1U : 0U;
        }
        else if (marker == 0xddU)
        {
            break;
        }
        else if (marker == 0xdaU)
        {
            if (!frame->width || !frame->height || !frame->quant[0] || !frame->quant[1] ||
                length != 2U + sizeof(scan_header) ||
                memcmp(body, scan_header, sizeof(scan_header)) != 0 || pos >= size - 2U)
                break;
            frame->scan = jpeg + pos;
            frame->scan_len = size - pos - 2U;
            return RT_TRUE;
        }
    }
    return RT_FALSE;
}

static uint32_t rtsp_now_ms(void)
{
    return (uint32_t)((uint64_t)rt_tick_get() * 1000U / RT_TICK_PER_SECOND);
}

static int rtsp_send_rtcp(struct rtsp_session *s, uint32_t now)
{
    /* A compound Sender Report and SDES CNAME on the negotiated RTCP channel. */
    static const char cname[] = "edgi-talk";
    uint8_t packet[52] = { '$', (uint8_t)(s->channel + 1U), 0, 48, 0x80, 200, 0, 6 };
    write_be32(packet + 8, s->id);
    write_be32(packet + 12, (uint32_t)time(RT_NULL) + 2208988800UL);
    write_be32(packet + 16, (uint32_t)(((uint64_t)(now % 1000U) << 32) / 1000U));
    write_be32(packet + 20, now * 90U);
    write_be32(packet + 24, s->packets_sent);
    write_be32(packet + 28, s->octets_sent);
    packet[32] = 0x81U;
    packet[33] = 202U;
    write_be16(packet + 34, 4U);
    write_be32(packet + 36, s->id);
    packet[40] = 1U;
    packet[41] = sizeof(cname) - 1U;
    memcpy(packet + 42, cname, sizeof(cname) - 1U);
    return rtsp_send(s->fd, packet, sizeof(packet));
}

static int rtsp_send_jpeg(struct rtsp_session *s, const struct rtp_jpeg *frame)
{
    uint8_t packet[24U + 132U + RTP_JPEG_FRAGMENT];
    uint32_t now = rtsp_now_ms();
    uint32_t timestamp = now * 90U;

    for (size_t off = 0; off < frame->scan_len;)
    {
        size_t payload = frame->scan_len - off;
        if (payload > RTP_JPEG_FRAGMENT)
            payload = RTP_JPEG_FRAGMENT;
        size_t header = 24U;
        packet[0] = '$';
        packet[1] = s->channel;
        packet[4] = 0x80U;
        packet[5] = (off + payload == frame->scan_len ? 0x80U : 0U) | RTP_JPEG_PAYLOAD_TYPE;
        write_be16(packet + 6, s->seq++);
        write_be32(packet + 8, timestamp);
        write_be32(packet + 12, s->id);
        write_be32(packet + 16, (uint32_t)off);
        packet[20] = frame->type;
        packet[21] = 255U;
        packet[22] = frame->width;
        packet[23] = frame->height;
        if (off == 0U)
        {
            write_be16(packet + 24, 0U);
            write_be16(packet + 26, 128U);
            memcpy(packet + 28, frame->quant[0], 64U);
            memcpy(packet + 92, frame->quant[1], 64U);
            header += 132U;
        }
        memcpy(packet + header, frame->scan + off, payload);
        write_be16(packet + 2, (uint16_t)(header - 4U + payload));
        if (rtsp_send(s->fd, packet, header + payload) != 0)
            return -1;
        s->packets_sent++;
        s->octets_sent += (uint32_t)(header - 16U + payload);
        off += payload;
    }
    ++g_rtsp_frames;
    if (s->packets_sent != 0U && (s->last_rtcp == 0U || now - s->last_rtcp >= 5000U))
    {
        s->last_rtcp = now;
        return rtsp_send_rtcp(s, now);
    }
    return 0;
}

static const char *rtsp_header(const char *request, const char *name)
{
    size_t len = strlen(name);
    const char *line = strchr(request, '\n');
    while (line != RT_NULL && line[1] != '\r' && line[1] != '\0')
    {
        ++line;
        if (strncasecmp(line, name, len) == 0 && line[len] == ':')
        {
            line += len + 1U;
            while (*line == ' ' || *line == '\t')
                ++line;
            return line;
        }
        line = strchr(line, '\n');
    }
    return RT_NULL;
}

static int rtsp_response(struct rtsp_session *s, const char *code, unsigned int cseq,
                         const char *headers, const char *body)
{
    char response[448];
    size_t size = body == RT_NULL ? 0U : strlen(body);
    int n = snprintf(response, sizeof(response), "RTSP/1.0 %s\r\nCSeq: %u\r\n%s"
                     "Content-Length: %u\r\n\r\n", code, cseq, headers, (unsigned int)size);
    if (n < 0 || (size_t)n >= sizeof(response) || rtsp_send(s->fd, response, (size_t)n) != 0)
        return -1;
    return size == 0U ? 0 : rtsp_send(s->fd, body, size);
}

static int rtsp_handle(struct rtsp_session *s, const char *request)
{
    char method[16], uri[96], version[12], headers[160];
    const char *value = rtsp_header(request, "CSeq");
    unsigned int cseq = value ? (unsigned int)strtoul(value, RT_NULL, 10) : 0U;
    if (value == RT_NULL || sscanf(request, "%15s %95s %11s", method, uri, version) != 3)
        return rtsp_response(s, "400 Bad Request", cseq, "", RT_NULL);
    if (strcmp(version, "RTSP/1.0") != 0)
        return rtsp_response(s, "505 RTSP Version Not Supported", cseq, "", RT_NULL);
    if (strcmp(method, "OPTIONS") == 0)
        return rtsp_response(s, "200 OK", cseq, RTSP_PUBLIC, RT_NULL);

    const char *path = uri;
    if (strncmp(uri, "rtsp://", 7U) == 0)
        path = strchr(uri + 7U, '/');
    if (path == RT_NULL ||
        (strcmp(path, RTSP_PATH) != 0 && strcmp(path, RTSP_PATH "/") != 0 &&
         strcmp(path, RTSP_PATH RTSP_CONTROL) != 0))
        return rtsp_response(s, "404 Not Found", cseq, "", RT_NULL);

    if (strcmp(method, "DESCRIBE") == 0)
    {
        static const char sdp[] =
            "v=0\r\no=- 0 0 IN IP4 0.0.0.0\r\ns=Edgi-Talk\r\nt=0 0\r\n"
            "a=control:*\r\nm=video 0 RTP/AVP 26\r\nc=IN IP4 0.0.0.0\r\n"
            "a=rtpmap:26 JPEG/90000\r\na=control:track1\r\n";
        snprintf(headers, sizeof(headers), "Content-Type: application/sdp\r\nContent-Base: %s/\r\n", s->url);
        return rtsp_response(s, "200 OK", cseq, headers, sdp);
    }

    if (strcmp(method, "SETUP") == 0)
    {
        value = rtsp_header(request, "Transport");
        unsigned int a = 0, b = 1;
        if (value == RT_NULL || strncasecmp(value, "RTP/AVP/TCP", 11U) != 0 ||
            (value[11] != ';' && value[11] != '\r'))
            return rtsp_response(s, "461 Unsupported Transport", cseq, "", RT_NULL);
        const char *transport_end = strchr(value, '\r');
        const char *interleave = strstr(value, "interleaved=");
        if (interleave != RT_NULL && transport_end != RT_NULL && interleave < transport_end &&
            (sscanf(interleave, "interleaved=%u-%u", &a, &b) != 2 || a >= 255U || b != a + 1U))
            return rtsp_response(s, "461 Unsupported Transport", cseq, "", RT_NULL);
        if (s->playing)
            return rtsp_response(s, "455 Method Not Valid in This State", cseq, "", RT_NULL);
        s->channel = (uint8_t)a;
        s->setup = RT_TRUE;
        snprintf(headers, sizeof(headers),
                 "Transport: RTP/AVP/TCP;unicast;interleaved=%u-%u\r\nSession: %08lx;timeout=%u\r\n",
                 a, b, (unsigned long)s->id, RTSP_SESSION_SECONDS);
        return rtsp_response(s, "200 OK", cseq, headers, RT_NULL);
    }

    rt_bool_t play = strcmp(method, "PLAY") == 0;
    rt_bool_t pause = strcmp(method, "PAUSE") == 0;
    rt_bool_t teardown = strcmp(method, "TEARDOWN") == 0;
    if (!play && !pause && !teardown && strcmp(method, "GET_PARAMETER") != 0)
        return rtsp_response(s, "405 Method Not Allowed", cseq, RTSP_PUBLIC, RT_NULL);
    value = rtsp_header(request, "Session");
    if (!s->setup || value == RT_NULL || strtoul(value, RT_NULL, 16) != s->id)
        return rtsp_response(s, "454 Session Not Found", cseq, "", RT_NULL);
    snprintf(headers, sizeof(headers), "Session: %08lx\r\n", (unsigned long)s->id);
    if (rtsp_response(s, "200 OK", cseq, headers, RT_NULL) != 0 || teardown)
        return -1;
    if (play || pause)
    {
        s->playing = play;
        g_rtsp_playing = play;
    }
    return 0;
}

static void rtsp_serve(int fd, uint32_t ip)
{
    char input[1536];
    struct in_addr address = { .s_addr = ip };
    uint32_t last_request = rtsp_now_ms();
    struct rtsp_session session = {
        .fd = fd, .id = last_request | 0x01000000U, .seq = (uint16_t)last_request,
    };
    size_t used = 0;
    struct timeval write_timeout = { .tv_sec = 0, .tv_usec = 250000 };
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &write_timeout, sizeof(write_timeout));
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    snprintf(session.url, sizeof(session.url), "rtsp://%s:%u" RTSP_PATH,
             inet_ntoa(address), WIFI_STREAM_RTSP_PORT);

    while (g_rtsp_enabled && wifi_mgr_get_station_ip() == ip &&
           rtsp_now_ms() - last_request < RTSP_SESSION_SECONDS * 1000U)
    {
        fd_set readable;
        struct timeval wait = { .tv_sec = 0, .tv_usec = session.playing ? 20000 : 200000 };
        FD_ZERO(&readable);
        FD_SET(fd, &readable);
        int ready = select(fd + 1, &readable, RT_NULL, RT_NULL, &wait);
        if (ready < 0)
            break;
        if (ready > 0 && FD_ISSET(fd, &readable))
        {
            int n = recv(fd, input + used, sizeof(input) - 1U - used, 0);
            if (n <= 0)
                break;
            used += (size_t)n;
            while (used != 0U)
            {
                size_t consumed;
                if (input[0] == '$')
                {
                    if (used < 4U)
                        break;
                    consumed = 4U + read_be16((const uint8_t *)input + 2U);
                    if (used < consumed)
                        break;
                }
                else
                {
                    input[used] = '\0';
                    char *end = strstr(input, "\r\n\r\n");
                    if (end == RT_NULL)
                        break;
                    consumed = (size_t)(end - input) + 4U;
                    const char *length = rtsp_header(input, "Content-Length");
                    unsigned long body_size = length ? strtoul(length, RT_NULL, 10) : 0U;
                    if (body_size > sizeof(input) - 1U - consumed)
                        return;
                    consumed += (size_t)body_size;
                    if (used < consumed)
                        break;
                    if (rtsp_handle(&session, input) != 0)
                        return;
                }
                last_request = rtsp_now_ms();
                used -= consumed;
                memmove(input, input + consumed, used);
            }
        }

        if (session.playing)
        {
            size_t size;
            int slot;
            const uint8_t *jpeg = wifi_stream_acquire_rtsp_jpeg(&session.jpeg_seq, &size, &slot);
            if (jpeg != RT_NULL)
            {
                struct rtp_jpeg frame = {0};
                int ret = rtsp_parse_jpeg(jpeg, size, &frame) ? rtsp_send_jpeg(&session, &frame) : 0;
                wifi_stream_release_rtsp_jpeg(slot);
                if (ret != 0)
                    break;
            }
        }
    }
}

static void rtsp_server(void *arg)
{
    (void)arg;
    int listener = -1;
    uint32_t bound = 0;
    for (;;)
    {
        uint32_t ip = g_rtsp_enabled ? wifi_mgr_get_station_ip() : 0U;
        if (listener >= 0 && ip != bound)
        {
            closesocket(listener);
            listener = -1;
        }
        if (ip == 0U)
        {
            rt_thread_mdelay(250);
            continue;
        }
        if (listener < 0)
        {
            struct sockaddr_in bind_addr = {
                .sin_family = AF_INET, .sin_port = htons(WIFI_STREAM_RTSP_PORT),
                .sin_addr.s_addr = ip,
            };
            listener = socket(AF_INET, SOCK_STREAM, 0);
            if (listener >= 0)
            {
                int reuse = 1;
                setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
                if (bind(listener, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) == 0 &&
                    listen(listener, 1) == 0)
                {
                    bound = ip;
                    LOG_I("LAN RTSP listening on :%u" RTSP_PATH, WIFI_STREAM_RTSP_PORT);
                }
                else
                {
                    closesocket(listener);
                    listener = -1;
                }
            }
            if (listener < 0)
            {
                rt_thread_mdelay(500);
                continue;
            }
        }
        fd_set readable;
        struct timeval wait = { .tv_sec = 0, .tv_usec = 250000 };
        FD_ZERO(&readable);
        FD_SET(listener, &readable);
        if (select(listener + 1, &readable, RT_NULL, RT_NULL, &wait) > 0 &&
            FD_ISSET(listener, &readable))
        {
            struct sockaddr_in peer;
            socklen_t size = sizeof(peer);
            int client = accept(listener, (struct sockaddr *)&peer, &size);
            if (client >= 0)
            {
                if (wifi_mgr_is_lan_peer(peer.sin_addr.s_addr))
                    rtsp_serve(client, bound);
                g_rtsp_playing = RT_FALSE;
                closesocket(client);
            }
        }
    }
}

static int rtsp_init(void)
{
    int ret = rt_thread_init(&g_rtsp_thread, "wf_rtsp", rtsp_server, RT_NULL,
                             g_rtsp_stack, sizeof(g_rtsp_stack), 19, 10);
    return ret == RT_EOK ? rt_thread_startup(&g_rtsp_thread) : ret;
}
INIT_APP_EXPORT(rtsp_init);

static void wifi_rtsp(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "start") == 0)
        g_rtsp_enabled = RT_TRUE;
    else if (argc > 1 && strcmp(argv[1], "stop") == 0)
        g_rtsp_enabled = RT_FALSE;

    struct in_addr address = { .s_addr = wifi_mgr_get_station_ip() };
    rt_kprintf("RTSP enabled=%d playing=%d frames=%lu rtsp://%s:%u" RTSP_PATH "\n",
               g_rtsp_enabled, wifi_rtsp_is_playing(), (unsigned long)g_rtsp_frames,
               inet_ntoa(address), WIFI_STREAM_RTSP_PORT);
}
MSH_CMD_EXPORT(wifi_rtsp, Show or enable/disable the LAN RTSP stream [start|stop]);
