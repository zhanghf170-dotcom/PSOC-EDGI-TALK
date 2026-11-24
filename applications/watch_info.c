#include <rtthread.h>
#include <arpa/inet.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include "watch_info.h"
#include "wifi_stream.h"

static watch_info_t watch_info = { .offset_minutes = 480, .weather_code = -1, .humidity = -1 };

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

static int watch_weather_update(unsigned long epoch, unsigned long observed, int code,
                                int temperature, int apparent, int humidity, int city,
                                watch_weather_source_t source)
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
    watch_info.city = city;
    watch_info.weather_epoch = (time_t)observed;
    watch_info.source = source;
    rt_hw_interrupt_enable(level);
    return RT_EOK;
}

static void watch_info_poll(const char *host)
{
    static const char request[] = "WATCH2";
    struct sockaddr_in address = { .sin_family = AF_INET, .sin_port = htons(WATCH_INFO_PORT) };
    struct timeval timeout = { .tv_sec = 2 };
    char reply[96];
    unsigned long epoch, weather_epoch;
    int code, temperature, apparent, humidity, city, used = 0;
    int sock, size = -1;

    if (inet_pton(AF_INET, host, &address.sin_addr) != 1 ||
        !wifi_mgr_is_lan_peer(address.sin_addr.s_addr))
        return;

    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0)
        return;

    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0 &&
        connect(sock, (struct sockaddr *)&address, sizeof(address)) == 0 &&
        send(sock, request, sizeof(request) - 1U, 0) == sizeof(request) - 1U)
        size = recv(sock, reply, sizeof(reply) - 1U, 0);
    closesocket(sock);

    if (size <= 0)
        return;
    reply[size] = '\0';
    if (sscanf(reply, "WATCH2 %lu %d %d %lu %d %d %d%n", &epoch, &code, &temperature,
               &weather_epoch, &apparent, &humidity, &city, &used) != 7 || used != size)
        return;

    if (code == -1)
        (void)watch_set_time(epoch);
    else
        (void)watch_weather_update(epoch, weather_epoch, code, temperature, apparent,
                                   humidity, city, WATCH_WEATHER_LAN);
}

static void watch_info_entry(void *parameter)
{
    wifi_status_t wifi;
    (void)parameter;

    for (;;)
    {
        wifi_mgr_get_status(&wifi);
        if (wifi.ready && wifi.relay_host[0] != '\0')
        {
            watch_info_poll(wifi.relay_host);
            rt_thread_mdelay(60000);
        }
        else
            rt_thread_mdelay(3000);
    }
}

int watch_info_init(void)
{
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
                             (int)values[2], (int)values[4], (int)values[5], (int)values[6],
                             WATCH_WEATHER_USB) != RT_EOK)
        return -RT_EINVAL;

    rt_kprintf("WEATHER OK city=%ld code=%ld temp10=%ld observed=%ld\n",
               values[6], values[1], values[2], values[3]);
    return RT_EOK;
}
MSH_CMD_EXPORT(watch_weather, Set Shanghai or selected city weather from USB);
