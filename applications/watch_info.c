#include <rtthread.h>
#include <arpa/inet.h>
#include <limits.h>
#include <math.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include "watch_info.h"
#include "wifi_stream.h"

#define DBG_TAG "watch.info"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

#define WATCH_CMA_HOST       "d1.weather.com.cn"
#define WATCH_OM_HOST        "api.open-meteo.com"
#define WATCH_HTTP_PORT      80U
#define WATCH_HTTP_BUF_SIZE  2048U
#define WATCH_SYNC_OK_MS     600000
#define WATCH_SYNC_LAN_MS    60000
#define WATCH_SYNC_RETRY_MS  10000
#define WATCH_SYNC_IDLE_MS   2000

typedef struct
{
    int city_id;
    char cma_code[16];
    char lat[16];
    char lon[16];
} watch_location_t;

static watch_info_t watch_info = { .offset_minutes = 480, .weather_code = -1, .humidity = -1, .wind_kmh_tenths = -1 };
static watch_location_t watch_loc = {
    .city_id = 0,
    .cma_code = "101020100",
    .lat = "31.2304",
    .lon = "121.4737",
};
static rt_sem_t watch_sync_sem = RT_NULL;

static int watch_set_time(unsigned long epoch)
{
    struct timeval tv = { .tv_sec = (time_t)epoch, .tv_usec = 0 };
    if (epoch < WATCH_EPOCH_MIN || epoch > LONG_MAX)
        return -RT_EINVAL;
    return settimeofday(&tv, RT_NULL) == 0 ? RT_EOK : -RT_ERROR;
}

void watch_info_get(watch_info_t *info)
{
    rt_base_t level = rt_hw_interrupt_disable();
    *info = watch_info;
    rt_hw_interrupt_enable(level);
}

static void watch_loc_get(watch_location_t *loc)
{
    rt_base_t level = rt_hw_interrupt_disable();
    *loc = watch_loc;
    rt_hw_interrupt_enable(level);
}

void watch_info_trigger_sync(void)
{
    if (watch_sync_sem != RT_NULL)
        (void)rt_sem_release(watch_sync_sem);
}

int watch_info_get_selected_city(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    int id = watch_loc.city_id;
    rt_hw_interrupt_enable(level);
    return id;
}

void watch_info_set_city(int city_id)
{
    static const struct {
        int id;
        const char *cma;
        const char *lat;
        const char *lon;
    } presets[3] = {
        { 0, "101020100", "31.2304", "121.4737" }, /* 上海 */
        { 1, "101010100", "39.9042", "116.4074" }, /* 北京 */
        { 2, "101280601", "22.5431", "114.0579" }, /* 深圳 */
    };
    if (city_id < 0 || city_id >= 3) return;

    watch_location_t next;
    rt_memset(&next, 0, sizeof(next));
    next.city_id = presets[city_id].id;
    rt_strncpy(next.cma_code, presets[city_id].cma, sizeof(next.cma_code) - 1U);
    rt_strncpy(next.lat, presets[city_id].lat, sizeof(next.lat) - 1U);
    rt_strncpy(next.lon, presets[city_id].lon, sizeof(next.lon) - 1U);

    rt_base_t level = rt_hw_interrupt_disable();
    watch_loc = next;
    watch_info.city = city_id;
    rt_hw_interrupt_enable(level);
    watch_info_trigger_sync();
}

static int watch_weather_update(unsigned long epoch, unsigned long observed, int code,
                                int temperature, int apparent, int humidity, int wind_kmh_tenths,
                                int city, watch_weather_source_t source)
{
    if (code < 0 || code > 99 || temperature < -1000 || temperature > 700 ||
        apparent < -1000 || apparent > 1000 || humidity < -1 || humidity > 100 ||
        city < 0 || city > 3 || observed < WATCH_EPOCH_MIN || observed > epoch ||
        watch_set_time(epoch) != RT_EOK)
        return -RT_EINVAL;

    rt_base_t level = rt_hw_interrupt_disable();
    watch_info.weather_code = code;
    watch_info.temperature_tenths = temperature;
    watch_info.apparent_tenths = apparent;
    watch_info.humidity = humidity;
    watch_info.wind_kmh_tenths = wind_kmh_tenths;
    watch_info.city = city;
    watch_info.weather_epoch = (time_t)observed;
    watch_info.source = source;
    rt_hw_interrupt_enable(level);
    return RT_EOK;
}

static long long watch_days_from_civil(int y, int m, int d)
{
    y -= (m <= 2) ? 1 : 0;
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153U * (unsigned)(m + (m > 2 ? -3 : 9)) + 2U) / 5U + (unsigned)d - 1U;
    unsigned doe = yoe * 365U + yoe / 4U - yoe / 100U + doy;
    return (long long)era * 146097LL + (long long)doe - 719468LL;
}

static rt_bool_t watch_parse_http_date(const char *headers, const char *headers_end, unsigned long *epoch_out)
{
    static const char *const months[12] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };
    const char *p = headers;

    while (p < headers_end)
    {
        const char *eol = strstr(p, "\r\n");
        if (eol == RT_NULL || eol > headers_end)
            eol = headers_end;

        if ((size_t)(eol - p) > 5U &&
            (p[0] == 'D' || p[0] == 'd') &&
            (p[1] == 'a' || p[1] == 'A') &&
            (p[2] == 't' || p[2] == 'T') &&
            (p[3] == 'e' || p[3] == 'E') &&
            p[4] == ':')
        {
            const char *v = p + 5;
            const char *comma = memchr(v, ',', (size_t)(eol - v));
            int day = 0, year = 0, hour = 0, min = 0, sec = 0, mon = 0;
            char mon_str[4] = {0};

            if (comma != RT_NULL)
                v = comma + 1;
            while (v < eol && (*v == ' ' || *v == '\t'))
                v++;

            if (sscanf(v, "%d %3s %d %d:%d:%d", &day, mon_str, &year, &hour, &min, &sec) == 6)
            {
                for (int i = 0; i < 12; i++)
                {
                    if (strncmp(mon_str, months[i], 3) == 0)
                    {
                        mon = i + 1;
                        break;
                    }
                }
                if (mon >= 1 && mon <= 12 && year >= 2020 && year <= 2099 &&
                    day >= 1 && day <= 31 && hour >= 0 && hour <= 23 &&
                    min >= 0 && min <= 59 && sec >= 0 && sec <= 60)
                {
                    long long ep = watch_days_from_civil(year, mon, day) * 86400LL +
                                   (long long)hour * 3600LL + (long long)min * 60LL + (long long)sec;
                    if (ep >= (long long)WATCH_EPOCH_MIN && ep <= (long long)LONG_MAX)
                    {
                        *epoch_out = (unsigned long)ep;
                        return RT_TRUE;
                    }
                }
            }
            return RT_FALSE;
        }

        if (eol >= headers_end)
            break;
        p = eol + 2;
    }

    return RT_FALSE;
}

static rt_bool_t watch_json_find_value(const char *obj_start, const char *obj_end,
                                       const char *key, const char **val_out)
{
    char pat[40];
    int plen = rt_snprintf(pat, sizeof(pat), "\"%s\":", key);
    if (plen <= 0 || (size_t)plen >= sizeof(pat))
        return RT_FALSE;

    const char *hit = strstr(obj_start, pat);
    if (hit == RT_NULL || hit >= obj_end)
        return RT_FALSE;
    hit += plen;
    while (hit < obj_end && (*hit == ' ' || *hit == '\t'))
        hit++;
    if (hit < obj_end)
    {
        *val_out = hit;
        return RT_TRUE;
    }
    return RT_FALSE;
}

static rt_bool_t watch_json_get_string(const char *obj_start, const char *obj_end,
                                       const char *key, char *out, size_t out_sz)
{
    const char *v = RT_NULL;
    size_t i = 0;

    if (out == RT_NULL || out_sz < 2U || !watch_json_find_value(obj_start, obj_end, key, &v))
        return RT_FALSE;
    if (*v != '"')
        return RT_FALSE;
    v++;
    while (v < obj_end && *v != '"' && i < out_sz - 1U)
    {
        if (*v == '\\' && (v + 1) < obj_end)
            v++;
        out[i++] = *v++;
    }
    out[i] = '\0';
    return i > 0U;
}

static rt_bool_t watch_json_get_double(const char *obj_start, const char *obj_end,
                                       const char *key, double *out)
{
    const char *v = RT_NULL;
    char *end = RT_NULL;
    double d;

    if (!watch_json_find_value(obj_start, obj_end, key, &v))
        return RT_FALSE;
    d = strtod(v, &end);
    if (end == v || end > obj_end)
        return RT_FALSE;
    *out = d;
    return RT_TRUE;
}

static rt_bool_t watch_json_get_ulong(const char *obj_start, const char *obj_end,
                                      const char *key, unsigned long *out)
{
    const char *v = RT_NULL;
    char *end = RT_NULL;
    unsigned long u;

    if (!watch_json_find_value(obj_start, obj_end, key, &v))
        return RT_FALSE;
    u = strtoul(v, &end, 10);
    if (end == v || end > obj_end)
        return RT_FALSE;
    *out = u;
    return RT_TRUE;
}

static int watch_round_tenths(double val)
{
    return (val >= 0.0) ? (int)(val * 10.0 + 0.5) : (int)(val * 10.0 - 0.5);
}

static int watch_http_get(const char *host, const char *req, int req_len, char *buf, size_t buf_sz)
{
    struct hostent *he;
    struct sockaddr_in addr;
    struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
    int sock, sent = 0, total = 0;

    he = gethostbyname(host);
    if (he == RT_NULL || he->h_addr_list == RT_NULL || he->h_addr_list[0] == RT_NULL)
    {
        LOG_W("DNS failed: %s", host);
        return -1;
    }

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
        return -1;

    (void)setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    (void)setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    rt_memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(WATCH_HTTP_PORT);
    rt_memcpy(&addr.sin_addr, he->h_addr_list[0], sizeof(addr.sin_addr));

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0)
    {
        LOG_W("HTTP connect failed: %s", host);
        closesocket(sock);
        return -1;
    }

    while (sent < req_len)
    {
        int n = send(sock, req + sent, (size_t)(req_len - sent), 0);
        if (n <= 0)
        {
            sent = -1;
            break;
        }
        sent += n;
    }

    if (sent == req_len)
    {
        while (total < (int)(buf_sz - 1U))
        {
            int n = recv(sock, buf + total, buf_sz - 1U - (size_t)total, 0);
            if (n <= 0)
                break;
            total += n;
        }
    }

    closesocket(sock);
    if (total <= 0)
    {
        LOG_W("HTTP empty/timeout: %s", host);
        return -1;
    }
    buf[total] = '\0';
    return total;
}

/* Map China Weather (weather.com.cn) dXX/nXX code to WMO weather code for UI */
static int watch_cma_to_wmo(const char *wcode)
{
    int c;
    if (wcode == RT_NULL || wcode[0] == '\0')
        return 0;
    if (wcode[0] == 'd' || wcode[0] == 'n' || wcode[0] == 'D' || wcode[0] == 'N')
        wcode++;
    c = atoi(wcode);
    switch (c)
    {
    case 0:
        return 0;  /* 晴 */
    case 1:
        return 2;  /* 多云 */
    case 2:
        return 3;  /* 阴 */
    case 3:
        return 80; /* 阵雨 */
    case 4:
    case 5:
        return 95; /* 雷暴 */
    case 7:
        return 51; /* 小雨/毛毛雨 */
    case 6:
    case 8:
    case 9:
    case 10:
    case 11:
    case 12:
    case 19:
    case 21:
    case 22:
    case 23:
    case 24:
    case 25:
    case 301:
        return 63; /* 雨 */
    case 13:
        return 85; /* 阵雪 */
    case 14:
    case 15:
    case 16:
    case 17:
    case 26:
    case 27:
    case 28:
    case 302:
        return 71; /* 雪 */
    case 18:
    case 20:
    case 29:
    case 30:
    case 31:
    case 32:
    case 49:
    case 53:
    case 54:
    case 55:
    case 56:
    case 57:
    case 58:
        return 45; /* 雾/霾 */
    default:
        return 2;
    }
}

static rt_bool_t watch_info_fetch_china_weather(void)
{
    watch_location_t loc;
    char req[256];
    char *buf;
    int req_len, total;
    rt_bool_t ok = RT_FALSE;

    watch_loc_get(&loc);
    if (loc.cma_code[0] == '\0')
        return RT_FALSE;

    buf = (char *)rt_malloc(WATCH_HTTP_BUF_SIZE);
    if (buf == RT_NULL)
        return RT_FALSE;

    req_len = rt_snprintf(
        req, sizeof(req),
        "GET /sk_2d/%s.html HTTP/1.0\r\n"
        "Host: %s\r\n"
        "Referer: http://www.weather.com.cn/\r\n"
        "User-Agent: Mozilla/5.0\r\n"
        "Connection: close\r\n\r\n",
        loc.cma_code, WATCH_CMA_HOST);

    total = (req_len > 0 && req_len < (int)sizeof(req)) ?
            watch_http_get(WATCH_CMA_HOST, req, req_len, buf, WATCH_HTTP_BUF_SIZE) : -1;

    if (total > 16)
    {
        const char *sep = strstr(buf, "\r\n\r\n");
        const char *status_ok = strstr(buf, " 200 ");
        if (sep != RT_NULL && strncmp(buf, "HTTP/1.", 7) == 0 &&
            status_ok != RT_NULL && status_ok < sep)
        {
            unsigned long epoch = 0;
            const char *obj_start = strchr(sep + 4, '{');
            const char *obj_end = obj_start ? strchr(obj_start, '}') : RT_NULL;

            if (watch_parse_http_date(buf, sep, &epoch) && obj_start != RT_NULL && obj_end != RT_NULL)
            {
                char temp_s[16] = {0}, sd_s[16] = {0}, wse_s[16] = {0};
                char wcode_s[16] = {0}, time_s[16] = {0};

                if (watch_json_get_string(obj_start, obj_end, "temp", temp_s, sizeof(temp_s)) &&
                    watch_json_get_string(obj_start, obj_end, "sd", sd_s, sizeof(sd_s)))
                {
                    char *end = RT_NULL;
                    double temp_c = strtod(temp_s, &end);
                    if (end != temp_s)
                    {
                        int humi = atoi(sd_s);
                        double wind_kmh = 0.0;
                        double wind_ms, vapor_e, app_c;
                        int temp10, app10, wmo_code;
                        unsigned long observed = epoch;

                        int wind10 = -1;
                        if (watch_json_get_string(obj_start, obj_end, "wse", wse_s, sizeof(wse_s)))
                        {
                            const char *wp = wse_s;
                            /* CMA wse can be "12km/h", "<12km/h", or HTML-escaped "&lt;12km/h" */
                            while (*wp != '\0' && (*wp < '0' || *wp > '9')) wp++;
                            if (*wp != '\0')
                            {
                                char *wend = RT_NULL;
                                double wv = strtod(wp, &wend);
                                if (wend != wp && wv >= 0.0 && wv <= 200.0)
                                {
                                    wind_kmh = wv;
                                    wind10 = watch_round_tenths(wv);
                                }
                            }
                        }
                        if (wind10 <= 0)
                        {
                            char ws_s[16] = {0};
                            if (watch_json_get_string(obj_start, obj_end, "WS", ws_s, sizeof(ws_s)))
                            {
                                const char *wp = ws_s;
                                while (*wp != '\0' && (*wp < '0' || *wp > '9')) wp++;
                                if (*wp != '\0')
                                {
                                    int lvl = atoi(wp);
                                    static const int bft_kmh[10] = { 2, 4, 9, 16, 24, 34, 44, 56, 68, 80 };
                                    if (lvl >= 0 && lvl <= 9)
                                    {
                                        wind_kmh = (double)bft_kmh[lvl];
                                        wind10 = bft_kmh[lvl] * 10;
                                    }
                                }
                            }
                        }
                        if (wind_kmh < 0.0 || wind_kmh > 200.0)
                            wind_kmh = 0.0;
                        if (humi < 0 || humi > 100)
                            humi = -1;

                        (void)watch_json_get_string(obj_start, obj_end, "weathercode", wcode_s, sizeof(wcode_s));
                        wmo_code = watch_cma_to_wmo(wcode_s);

                        /* Steadman apparent temperature from T(C), RH(%), wind(m/s) */
                        wind_ms = wind_kmh / 3.6;
                        vapor_e = (humi >= 0 ? (double)humi / 100.0 : 0.5) * 6.105 *
                                  exp((17.27 * temp_c) / (237.7 + temp_c));
                        app_c = temp_c + 0.33 * vapor_e - 0.70 * wind_ms - 4.00;
                        temp10 = watch_round_tenths(temp_c);
                        app10 = watch_round_tenths(app_c);

                        if (watch_json_get_string(obj_start, obj_end, "time", time_s, sizeof(time_s)))
                        {
                            int oh = 0, om = 0;
                            if (sscanf(time_s, "%d:%d", &oh, &om) == 2 &&
                                oh >= 0 && oh <= 23 && om >= 0 && om <= 59)
                            {
                                unsigned long local_now = epoch + 28800UL;
                                unsigned long local_day = local_now - (local_now % 86400UL);
                                unsigned long obs_local = local_day + (unsigned long)oh * 3600UL +
                                                          (unsigned long)om * 60UL;
                                if (obs_local > local_now + 600UL)
                                    obs_local -= 86400UL;
                                if (obs_local >= 28800UL && obs_local <= local_now &&
                                    (local_now - obs_local) <= 3600UL)
                                {
                                    observed = obs_local - 28800UL;
                                }
                            }
                        }

                        if (watch_weather_update(epoch, observed, wmo_code, temp10, app10,
                                                 humi, wind10, loc.city_id, WATCH_WEATHER_WIFI) == RT_EOK)
                        {
                            int abs_t = temp10 < 0 ? -temp10 : temp10;
                            LOG_I("Wi-Fi weather synced (CMA): city=%d wcode=%s->%d temp=%s%d.%dC humi=%d%% epoch=%lu",
                                  loc.city_id, wcode_s, wmo_code, temp10 < 0 ? "-" : "",
                                  abs_t / 10, abs_t % 10, humi, epoch);
                            ok = RT_TRUE;
                        }
                    }
                }
            }
        }
        else
        {
            LOG_W("CMA HTTP bad status");
        }
    }

    rt_free(buf);
    return ok;
}

static rt_bool_t watch_info_fetch_open_meteo(void)
{
    watch_location_t loc;
    char req[320];
    char *buf;
    int req_len, total;
    rt_bool_t ok = RT_FALSE;

    watch_loc_get(&loc);
    if (loc.lat[0] == '\0' || loc.lon[0] == '\0')
        return RT_FALSE;

    buf = (char *)rt_malloc(WATCH_HTTP_BUF_SIZE);
    if (buf == RT_NULL)
        return RT_FALSE;

    req_len = rt_snprintf(
        req, sizeof(req),
        "GET /v1/forecast?latitude=%s&longitude=%s"
        "&current=temperature_2m,weather_code,apparent_temperature,relative_humidity_2m,wind_speed_10m"
        "&timeformat=unixtime HTTP/1.0\r\n"
        "Host: %s\r\n"
        "User-Agent: EdgiTalk/1.0\r\n"
        "Connection: close\r\n\r\n",
        loc.lat, loc.lon, WATCH_OM_HOST);

    total = (req_len > 0 && req_len < (int)sizeof(req)) ?
            watch_http_get(WATCH_OM_HOST, req, req_len, buf, WATCH_HTTP_BUF_SIZE) : -1;

    if (total > 16)
    {
        const char *sep = strstr(buf, "\r\n\r\n");
        const char *status_ok = strstr(buf, " 200 ");
        if (sep != RT_NULL && strncmp(buf, "HTTP/1.", 7) == 0 &&
            status_ok != RT_NULL && status_ok < sep)
        {
            const char *cur = strstr(sep + 4, "\"current\":{");
            if (cur != RT_NULL)
            {
                const char *obj_start = cur + 10;
                const char *obj_end = strchr(obj_start, '}');
                unsigned long observed = 0, epoch = 0;
                double temp_c = 0.0, app_c = 0.0, humi_d = 0.0, code_d = -1.0;

                if (obj_end != RT_NULL &&
                    watch_json_get_ulong(obj_start, obj_end, "time", &observed) &&
                    watch_json_get_double(obj_start, obj_end, "weather_code", &code_d) &&
                    watch_json_get_double(obj_start, obj_end, "temperature_2m", &temp_c) &&
                    watch_json_get_double(obj_start, obj_end, "apparent_temperature", &app_c) &&
                    watch_json_get_double(obj_start, obj_end, "relative_humidity_2m", &humi_d))
                {
                    int code = (int)(code_d + 0.5);
                    int temp10 = watch_round_tenths(temp_c);
                    int app10 = watch_round_tenths(app_c);
                    int humi = (int)(humi_d + 0.5);
                    double wind_d = -1.0;
                    int wind10 = watch_json_get_double(obj_start, obj_end, "wind_speed_10m", &wind_d)
                                     ? watch_round_tenths(wind_d) : -1;

                    if (!watch_parse_http_date(buf, sep, &epoch))
                        epoch = observed;
                    if (observed > epoch && (observed - epoch) <= 900UL)
                        observed = epoch;

                    if (watch_weather_update(epoch, observed, code, temp10, app10,
                                             humi, wind10, loc.city_id, WATCH_WEATHER_WIFI) == RT_EOK)
                    {
                        int abs_t = temp10 < 0 ? -temp10 : temp10;
                        LOG_I("Wi-Fi weather synced (OM): city=%d code=%d temp=%s%d.%dC humi=%d%% epoch=%lu",
                              loc.city_id, code, temp10 < 0 ? "-" : "", abs_t / 10, abs_t % 10, humi, epoch);
                        ok = RT_TRUE;
                    }
                }
            }
        }
    }

    rt_free(buf);
    return ok;
}

static rt_bool_t watch_info_poll(const char *host)
{
    static const char request[] = "WATCH2";
    struct sockaddr_in address = { .sin_family = AF_INET, .sin_port = htons(WATCH_INFO_PORT) };
    struct sockaddr_in peer;
    socklen_t peer_len = sizeof(peer);
    struct timeval timeout = { .tv_sec = 2 };
    rt_bool_t use_broadcast = (host == RT_NULL || host[0] == '\0');
    char reply[96];
    unsigned long epoch, weather_epoch;
    int code, temperature, apparent, humidity, city, used = 0;
    int sock, size = -1;

    if (use_broadcast)
    {
        uint32_t bcast = wifi_mgr_get_lan_broadcast();
        if (bcast == 0U)
            return RT_FALSE;
        address.sin_addr.s_addr = bcast;
    }
    else if (inet_pton(AF_INET, host, &address.sin_addr) != 1 ||
             !wifi_mgr_is_lan_peer(address.sin_addr.s_addr))
    {
        return RT_FALSE;
    }

    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0)
        return RT_FALSE;

    if (use_broadcast)
    {
        int broadcast = 1;
        if (setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast)) != 0)
        {
            closesocket(sock);
            return RT_FALSE;
        }
    }

    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0 &&
        sendto(sock, request, sizeof(request) - 1U, 0,
               (struct sockaddr *)&address, sizeof(address)) == (int)(sizeof(request) - 1U))
    {
        size = recvfrom(sock, reply, sizeof(reply) - 1U, 0,
                        (struct sockaddr *)&peer, &peer_len);
    }
    closesocket(sock);

    if (size <= 0 || peer.sin_family != AF_INET || !wifi_mgr_is_lan_peer(peer.sin_addr.s_addr))
        return RT_FALSE;
    if (!use_broadcast && peer.sin_addr.s_addr != address.sin_addr.s_addr)
        return RT_FALSE;

    reply[size] = '\0';
    if (sscanf(reply, "WATCH2 %lu %d %d %lu %d %d %d%n", &epoch, &code, &temperature,
               &weather_epoch, &apparent, &humidity, &city, &used) != 7 || used != size)
        return RT_FALSE;

    if (use_broadcast)
    {
        const char *peer_ip = inet_ntoa(peer.sin_addr);
        if (peer_ip != RT_NULL && peer_ip[0] != '\0')
            (void)wifi_stream_set_relay_config(peer_ip, 0U, RT_TRUE);
    }

    if (code == -1)
    {
        (void)watch_set_time(epoch);
        return RT_FALSE;
    }
    return watch_weather_update(epoch, weather_epoch, code, temperature, apparent,
                                humidity, -1, city, WATCH_WEATHER_LAN) == RT_EOK;
}

static void watch_info_entry(void *parameter)
{
    wifi_status_t wifi;
    (void)parameter;

    for (;;)
    {
        rt_int32_t wait_ms = WATCH_SYNC_IDLE_MS;

        wifi_mgr_get_status(&wifi);
        if (wifi.ready)
        {
            rt_bool_t synced = RT_FALSE;
            if (watch_info_fetch_china_weather())
            {
                synced = RT_TRUE;
                wait_ms = WATCH_SYNC_OK_MS;
            }
            else if (watch_info_fetch_open_meteo())
            {
                synced = RT_TRUE;
                wait_ms = WATCH_SYNC_OK_MS;
            }
            else if (watch_info_poll(wifi.relay_host[0] != '\0' ? wifi.relay_host : RT_NULL))
            {
                synced = RT_TRUE;
                wait_ms = WATCH_SYNC_LAN_MS;
            }

            if (!synced)
                wait_ms = WATCH_SYNC_RETRY_MS;
        }

        if (watch_sync_sem != RT_NULL)
            (void)rt_sem_take(watch_sync_sem, rt_tick_from_millisecond(wait_ms));
        else
            rt_thread_mdelay(wait_ms);
    }
}

int watch_info_init(void)
{
    watch_sync_sem = rt_sem_create("watch_s", 0, RT_IPC_FLAG_FIFO);
    rt_thread_t thread = rt_thread_create("watch", watch_info_entry, RT_NULL, 4096, 19, 10);
    return thread != RT_NULL ? rt_thread_startup(thread) : -RT_ENOMEM;
}

static int watch_time(int argc, char **argv)
{
    char *end;
    unsigned long epoch;
    long offset;

    if (argc == 2 || argc == 3)
    {
        epoch = strtoul(argv[1], &end, 10);
        if (end != argv[1] && *end == '\0')
        {
            offset = 480;
            if (argc == 3)
            {
                offset = strtol(argv[2], &end, 10);
                if (end == argv[2] || *end != '\0' || offset < -720 || offset > 840)
                    return -RT_EINVAL;
            }

            if (watch_set_time(epoch) != RT_EOK)
                return -RT_EINVAL;
            rt_base_t level = rt_hw_interrupt_disable();
            watch_info.offset_minutes = (int)offset;
            rt_hw_interrupt_enable(level);
        }
        else
            return -RT_EINVAL;
    }
    else if (argc != 1)
        return -RT_EINVAL;

    rt_kprintf("UTC epoch: %ld, offset: %d minutes\n", (long)time(RT_NULL), watch_info.offset_minutes);
    return RT_EOK;
}
MSH_CMD_EXPORT(watch_time, watch_time [utc_epoch [offset_minutes]]);

static int watch_weather(int argc, char **argv)
{
    long values[7];
    for (int i = 1; i < argc && i <= 7; i++)
    {
        char *end;
        values[i - 1] = strtol(argv[i], &end, 10);
        if (end == argv[i] || *end != '\0')
            return -RT_EINVAL;
    }
    if (argc != 8 ||
        watch_weather_update((unsigned long)values[0], (unsigned long)values[3], (int)values[1],
                             (int)values[2], (int)values[4], (int)values[5], -1, (int)values[6],
                             WATCH_WEATHER_USB) != RT_EOK)
        return -RT_EINVAL;

    rt_kprintf("WEATHER OK city=%ld code=%ld temp10=%ld observed=%ld\n",
               values[6], values[1], values[2], values[3]);
    return RT_EOK;
}
MSH_CMD_EXPORT(watch_weather, Set Shanghai or selected city weather from USB);

static int watch_city(int argc, char **argv)
{
    watch_location_t next;
    rt_memset(&next, 0, sizeof(next));

    if (argc == 2)
    {
        if (strcmp(argv[1], "shanghai") == 0)
        {
            next.city_id = 0;
            rt_strncpy(next.cma_code, "101020100", sizeof(next.cma_code) - 1U);
            rt_strncpy(next.lat, "31.2304", sizeof(next.lat) - 1U);
            rt_strncpy(next.lon, "121.4737", sizeof(next.lon) - 1U);
        }
        else if (strcmp(argv[1], "beijing") == 0)
        {
            next.city_id = 1;
            rt_strncpy(next.cma_code, "101010100", sizeof(next.cma_code) - 1U);
            rt_strncpy(next.lat, "39.9042", sizeof(next.lat) - 1U);
            rt_strncpy(next.lon, "116.4074", sizeof(next.lon) - 1U);
        }
        else if (strcmp(argv[1], "shenzhen") == 0)
        {
            next.city_id = 2;
            rt_strncpy(next.cma_code, "101280601", sizeof(next.cma_code) - 1U);
            rt_strncpy(next.lat, "22.5431", sizeof(next.lat) - 1U);
            rt_strncpy(next.lon, "114.0579", sizeof(next.lon) - 1U);
        }
        else if (strlen(argv[1]) == 9U)
        {
            for (size_t i = 0; i < 9U; i++)
            {
                if (argv[1][i] < '0' || argv[1][i] > '9')
                    return -RT_EINVAL;
            }
            next.city_id = 3;
            rt_strncpy(next.cma_code, argv[1], sizeof(next.cma_code) - 1U);
        }
        else
        {
            rt_kprintf("Usage: watch_city <shanghai|beijing|shenzhen|9-digit-code> | watch_city <lat> <lon>\n");
            return -RT_EINVAL;
        }
    }
    else if (argc == 3)
    {
        char *end1 = RT_NULL, *end2 = RT_NULL;
        double lat = strtod(argv[1], &end1);
        double lon = strtod(argv[2], &end2);
        if (end1 == argv[1] || *end1 != '\0' || end2 == argv[2] || *end2 != '\0' ||
            lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0)
        {
            rt_kprintf("Invalid coordinates\n");
            return -RT_EINVAL;
        }
        next.city_id = 3;
        rt_strncpy(next.lat, argv[1], sizeof(next.lat) - 1U);
        rt_strncpy(next.lon, argv[2], sizeof(next.lon) - 1U);
    }
    else
    {
        watch_loc_get(&next);
        rt_kprintf("city=%d cma=%s lat=%s lon=%s\n", next.city_id,
                   next.cma_code[0] ? next.cma_code : "-",
                   next.lat[0] ? next.lat : "-",
                   next.lon[0] ? next.lon : "-");
        return RT_EOK;
    }

    rt_base_t level = rt_hw_interrupt_disable();
    watch_loc = next;
    rt_hw_interrupt_enable(level);
    watch_info_trigger_sync();
    rt_kprintf("watch_city -> id=%d cma=%s, sync triggered\n", next.city_id,
               next.cma_code[0] ? next.cma_code : "-");
    return RT_EOK;
}
MSH_CMD_EXPORT(watch_city, Select weather city: shanghai|beijing|shenzhen|9-digit-code|lat lon);

static int watch_sync(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    watch_info_trigger_sync();
    rt_kprintf("watch sync triggered\n");
    return RT_EOK;
}
MSH_CMD_EXPORT(watch_sync, Trigger immediate weather and time sync);

static int watch_status(int argc, char **argv)
{
    watch_info_t info;
    wifi_status_t wifi;
    static const char *const src_names[] = { "none", "lan", "usb", "wifi" };
    const char *src;
    (void)argc;
    (void)argv;

    watch_info_get(&info);
    wifi_mgr_get_status(&wifi);
    src = (info.source <= WATCH_WEATHER_WIFI) ? src_names[info.source] : "none";
    rt_kprintf("epoch=%ld offset=%d source=%s city=%d code=%d temp10=%d apparent10=%d humi=%d observed=%ld relay=%s\n",
               (long)time(RT_NULL), info.offset_minutes, src, info.city, info.weather_code,
               info.temperature_tenths, info.apparent_tenths, info.humidity,
               (long)info.weather_epoch, wifi.relay_host[0] ? wifi.relay_host : "auto");
    return RT_EOK;
}
MSH_CMD_EXPORT(watch_status, Show current watch clock and weather sync status);
