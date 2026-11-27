/*
 * Copyright (c) 2006-2025, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Alert Manager — non-blocking danger alerts, persistent SD log & audio alarm
 */

#include <rtthread.h>
#include <rtdevice.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wavplayer.h>
#include "alert_manager.h"
#include "wifi_stream.h"
#include "xiaozhi.h"
#include "sd_assets.h"
#include "music_player.h"

#define DBG_TAG              "alert"
#define DBG_LVL              DBG_INFO
#include <rtdbg.h>

#define COOLDOWN_DEFAULT_MS  3000U
#define ALERT_VOLUME_DEFAULT 75
#define LOG_PATH_DEFAULT     "/sdcard/danger_log.txt"
#define ALERT_WAV_PATH       SD_ALARM_WAV_PATH

#define ALERT_MSG_DANGER     1U
#define ALERT_MSG_TEST_SOUND 2U
#define ALERT_MSG_STOP       3U
#define ALERT_MQ_MAX_MSGS    8U

typedef struct
{
    uint8_t  type;
    uint32_t seq;
    rt_tick_t tick;
    float    conf;
    float    temp;
    float    humi;
} alert_msg_t;

static struct
{
    rt_bool_t  init;
    rt_tick_t  last_tick;
    rt_tick_t  boot_tick;
    uint32_t   cooldown_ms;
    uint32_t   danger_count;
    uint32_t   live_session_alerts;
    float      threshold;
    int        volume;
    rt_mq_t    mq;
    rt_thread_t worker;
    struct rt_mutex log_lock;
} g_ctx;

/* Parse max NO.<n> or line count from persistent SD danger log */
static uint32_t alert_scan_sd_log_count(void)
{
    FILE *fp;
    char line[192];
    uint32_t max_no = 0U;
    uint32_t lines = 0U;

    fp = fopen(LOG_PATH_DEFAULT, "r");
    if (fp == NULL)
    {
        return 0U;
    }

    while (fgets(line, sizeof(line), fp) != NULL)
    {
        unsigned long no = 0UL;
        if (sscanf(line, "NO.%lu", &no) == 1)
        {
            if ((uint32_t)no > max_no)
            {
                max_no = (uint32_t)no;
            }
        }
        if (line[0] != '\n' && line[0] != '\r' && line[0] != '\0')
        {
            lines++;
        }
    }

    fclose(fp);
    return (max_no > lines) ? max_no : lines;
}

/* Write danger record to SD card from dedicated worker thread */
static void alert_write_danger_log(uint32_t seq, rt_tick_t evt_tick,
                                   float confidence, float temp, float humi)
{
    FILE *fp;
    uint32_t sec, h, m, s;

    rt_mutex_take(&g_ctx.log_lock, RT_WAITING_FOREVER);
    fp = fopen(LOG_PATH_DEFAULT, "a");
    if (fp == NULL)
    {
        rt_mutex_release(&g_ctx.log_lock);
        LOG_W("open danger log failed");
        return;
    }

    sec = (uint32_t)((evt_tick - g_ctx.boot_tick) / RT_TICK_PER_SECOND);
    h   = sec / 3600U;
    m   = (sec % 3600U) / 60U;
    s   = sec % 60U;

    {
        char rec[224];
        float conf_disp = isfinite(confidence) ? (confidence * 100.0f) : 0.0f;
        float temp_disp = isfinite(temp) ? temp : 0.0f;
        float humi_disp = isfinite(humi) ? humi : 0.0f;
        rt_snprintf(rec, sizeof(rec),
                    "NO.%lu | Conf:%.0f%% | Temp:%.1fC | Humi:%.1f%% | "
                    "Time:[%02lu:%02lu:%02lu] | "
                    "Do not drive while drowsy! Please rest.\n",
                    (unsigned long)seq,
                    (double)conf_disp,
                    (double)temp_disp, (double)humi_disp,
                    (unsigned long)h, (unsigned long)m, (unsigned long)s);
        fputs(rec, fp);
    }

    fclose(fp);
    rt_mutex_release(&g_ctx.log_lock);
}

static void alert_load_volume_cfg(void)
{
    FILE *fp = fopen(SD_AUDIO_CFG_PATH, "r");
    char line[64];
    if (fp == NULL)
    {
        return;
    }
    while (fgets(line, sizeof(line), fp) != NULL)
    {
        int v = 0;
        if (sscanf(line, "volume=%d", &v) == 1)
        {
            alert_set_volume(v);
            LOG_I("restored volume=%d from %s", g_ctx.volume, SD_AUDIO_CFG_PATH);
            break;
        }
    }
    fclose(fp);
}

void alert_save_volume_cfg(void)
{
    FILE *fp;
    if (!sdcard_is_mounted())
    {
        return;
    }
    fp = fopen(SD_AUDIO_CFG_PATH, "w");
    if (fp != NULL)
    {
        char line[32];
        rt_snprintf(line, sizeof(line), "volume=%d\n", g_ctx.volume);
        fputs(line, fp);
        fclose(fp);
    }
}

static void alert_worker_entry(void *parameter)
{
    alert_msg_t msg;
    uint32_t restored;

    (void)parameter;

    /* Wait briefly for mnt_init() thread to mount /sdcard, then restore persistent count & volume */
    (void)sdcard_wait_mounted(3000);

    if (sdcard_is_mounted())
    {
        alert_load_volume_cfg();
    }

    rt_mutex_take(&g_ctx.log_lock, RT_WAITING_FOREVER);
    restored = alert_scan_sd_log_count();
    if (restored > 0U)
    {
        rt_base_t level = rt_hw_interrupt_disable();
        g_ctx.danger_count = restored + g_ctx.live_session_alerts;
        rt_hw_interrupt_enable(level);
        LOG_I("restored %lu historical danger records (total=%lu)",
              (unsigned long)restored, (unsigned long)g_ctx.danger_count);
    }
    rt_mutex_release(&g_ctx.log_lock);

    while (1)
    {
        if (rt_mq_recv(g_ctx.mq, &msg, sizeof(msg), RT_WAITING_FOREVER) != sizeof(msg))
        {
            continue;
        }

        if (msg.type == ALERT_MSG_DANGER)
        {
            xiaozhi_notify_fatigue_alert(msg.conf);
            music_player_preempt();
            xiaozhi_audio_lock();
            wavplayer_volume_set(g_ctx.volume);
            if (sdcard_is_mounted() && access(ALERT_WAV_PATH, 0) == 0)
            {
                if (wavplayer_play(ALERT_WAV_PATH) == RT_EOK)
                {
                    for (int w = 0; w < 80; w++)
                    {
                        rt_thread_mdelay(25);
                        if (wavplayer_state_get() == PLAYER_STATE_STOPED) break;
                    }
                    (void)wavplayer_stop();
                }
            }
            else
            {
                LOG_W("alarm wav missing on TF card: %s", ALERT_WAV_PATH);
            }
            xiaozhi_audio_unlock();
            /* Follow alarm tone with Gemini-chan cute female voice fatigue warning */
            xiaozhi_speak_clip(XZ_VOICE_ALERT);
            alert_write_danger_log(msg.seq, msg.tick, msg.conf, msg.temp, msg.humi);
        }
        else if (msg.type == ALERT_MSG_TEST_SOUND)
        {
            music_player_preempt();
            xiaozhi_audio_preempt_for_alarm();
            xiaozhi_audio_lock();
            wavplayer_volume_set(g_ctx.volume);
            if (sdcard_is_mounted() && access(ALERT_WAV_PATH, 0) == 0)
            {
                if (wavplayer_play(ALERT_WAV_PATH) == RT_EOK)
                {
                    for (int w = 0; w < 80; w++)
                    {
                        rt_thread_mdelay(25);
                        if (wavplayer_state_get() == PLAYER_STATE_STOPED) break;
                    }
                    (void)wavplayer_stop();
                }
            }
            else
            {
                LOG_W("alarm wav missing on TF card: %s", ALERT_WAV_PATH);
            }
            xiaozhi_audio_unlock();
        }
        else if (msg.type == ALERT_MSG_STOP)
        {
            music_player_preempt();
            xiaozhi_audio_preempt_for_alarm();
            (void)wavplayer_stop();
        }
    }
}

int alert_manager_init(void)
{
    if (g_ctx.init)
    {
        return RT_EOK;
    }

    rt_memset(&g_ctx, 0, sizeof(g_ctx));
    g_ctx.boot_tick   = rt_tick_get();
    g_ctx.cooldown_ms = COOLDOWN_DEFAULT_MS;
    g_ctx.threshold   = ALERT_THRESHOLD;
    g_ctx.volume      = ALERT_VOLUME_DEFAULT;
    rt_mutex_init(&g_ctx.log_lock, "alrt_lk", RT_IPC_FLAG_PRIO);

    g_ctx.mq = rt_mq_create("alert_q", sizeof(alert_msg_t), ALERT_MQ_MAX_MSGS, RT_IPC_FLAG_FIFO);
    if (g_ctx.mq == RT_NULL)
    {
        LOG_E("create alert mq failed");
        return -RT_ENOMEM;
    }

    g_ctx.worker = rt_thread_create("alert_wk", alert_worker_entry, RT_NULL,
                                    4096, 14, 10);
    if (g_ctx.worker == RT_NULL)
    {
        rt_mq_delete(g_ctx.mq);
        g_ctx.mq = RT_NULL;
        LOG_E("create alert worker failed");
        return -RT_ENOMEM;
    }

    g_ctx.init = RT_TRUE;
    wavplayer_volume_set(g_ctx.volume);
    rt_thread_startup(g_ctx.worker);

    LOG_I("ready (async, threshold=%.0f%%, vol=%d, cooldown=%ums, wav=%s)",
          (double)(g_ctx.threshold * 100.0f), g_ctx.volume,
          g_ctx.cooldown_ms, ALERT_WAV_PATH);
    return RT_EOK;
}

int alert_danger(float confidence, float temp, float humi)
{
    alert_msg_t msg;
    rt_base_t level;
    rt_tick_t now;
    uint32_t seq;

    if (!g_ctx.init)
    {
        return -RT_ERROR;
    }

    now = rt_tick_get();

    level = rt_hw_interrupt_disable();
    if ((g_ctx.cooldown_ms > 0U) && (g_ctx.last_tick != 0U))
    {
        uint32_t elapsed = ((uint32_t)(now - g_ctx.last_tick)) *
                           (1000U / RT_TICK_PER_SECOND);
        if (elapsed < g_ctx.cooldown_ms)
        {
            rt_hw_interrupt_enable(level);
            return RT_EOK;
        }
    }

    g_ctx.live_session_alerts++;
    g_ctx.danger_count++;
    g_ctx.last_tick = now;
    seq = g_ctx.danger_count;
    rt_hw_interrupt_enable(level);

    LOG_I("DANGER #%lu (%.0f%%, T=%.1fC H=%.1f%%)",
          (unsigned long)seq, (double)(confidence * 100.0f),
          (double)temp, (double)humi);

    /* Non-blocking notification to Wi-Fi TCP relay & encoder */
    wifi_stream_notify_danger(seq, confidence);

    rt_memset(&msg, 0, sizeof(msg));
    msg.type = ALERT_MSG_DANGER;
    msg.seq  = seq;
    msg.tick = now;
    msg.conf = confidence;
    msg.temp = temp;
    msg.humi = humi;

    if (g_ctx.mq != RT_NULL)
    {
        rt_err_t ret = rt_mq_send(g_ctx.mq, &msg, sizeof(msg));
        if (ret != RT_EOK)
        {
            LOG_W("alert queue full (%d)", ret);
            return ret;
        }
    }

    return RT_EOK;
}

void alert_stop(void)
{
    alert_msg_t msg;

    if (!g_ctx.init || g_ctx.mq == RT_NULL)
    {
        return;
    }

    rt_memset(&msg, 0, sizeof(msg));
    msg.type = ALERT_MSG_STOP;
    (void)rt_mq_send(g_ctx.mq, &msg, sizeof(msg));
}

int alert_test_sound(void)
{
    alert_msg_t msg;

    if (!g_ctx.init || g_ctx.mq == RT_NULL)
    {
        return -RT_ERROR;
    }

    rt_memset(&msg, 0, sizeof(msg));
    msg.type = ALERT_MSG_TEST_SOUND;
    return rt_mq_send(g_ctx.mq, &msg, sizeof(msg));
}

MSH_CMD_EXPORT(alert_test_sound, Play the alarm stored in firmware);

void alert_set_cooldown_ms(uint32_t ms)
{
    g_ctx.cooldown_ms = ms;
}

uint32_t alert_get_cooldown_ms(void)
{
    return g_ctx.cooldown_ms;
}

uint32_t alert_get_danger_count(void)
{
    return g_ctx.danger_count;
}

rt_tick_t alert_get_last_danger_tick(void)
{
    return g_ctx.last_tick;
}

void alert_set_threshold(float thresh)
{
    if (thresh < 0.50f) thresh = 0.50f;
    if (thresh > 0.98f) thresh = 0.98f;
    g_ctx.threshold = thresh;
}

float alert_get_threshold(void)
{
    return g_ctx.init ? g_ctx.threshold : ALERT_THRESHOLD;
}

void alert_set_volume(int vol)
{
    rt_device_t snd;
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;
    g_ctx.volume = vol;
    wavplayer_volume_set(vol > 99 ? 99 : vol);

    snd = rt_device_find(PKG_WP_PLAY_DEVICE);
    if (snd != RT_NULL)
    {
        struct rt_audio_caps caps;
        rt_memset(&caps, 0, sizeof(caps));
        caps.main_type = AUDIO_TYPE_MIXER;
        caps.sub_type  = AUDIO_MIXER_VOLUME;
        caps.udata.value = vol;
        (void)rt_device_control(snd, AUDIO_CTL_CONFIGURE, &caps);
    }
}

int alert_get_volume(void)
{
    return g_ctx.volume;
}

int alert_read_recent_logs(char *buf, size_t buf_size, int max_lines)
{
    FILE *fp;
    char line[192];
    char *ring[16];
    int cap;
    int total = 0;
    int idx;
    size_t pos = 0U;

    if ((buf == NULL) || (buf_size == 0U))
    {
        return -RT_EINVAL;
    }

    buf[0] = '\0';
    cap = (max_lines > 0 && max_lines <= 16) ? max_lines : 8;
    rt_memset(ring, 0, sizeof(ring));

    rt_mutex_take(&g_ctx.log_lock, RT_WAITING_FOREVER);
    fp = fopen(LOG_PATH_DEFAULT, "r");
    if (fp == NULL)
    {
        rt_mutex_release(&g_ctx.log_lock);
        rt_snprintf(buf, buf_size, "No danger logs recorded on /sdcard.");
        return 0;
    }

    while (fgets(line, sizeof(line), fp) != NULL)
    {
        size_t len = strlen(line);
        if (len == 0U) continue;
        idx = total % cap;
        if (ring[idx] == NULL)
        {
            ring[idx] = (char *)rt_malloc(sizeof(line));
        }
        if (ring[idx] != NULL)
        {
            rt_strncpy(ring[idx], line, sizeof(line) - 1U);
            ring[idx][sizeof(line) - 1U] = '\0';
        }
        total++;
    }
    fclose(fp);
    rt_mutex_release(&g_ctx.log_lock);

    if (total == 0)
    {
        rt_snprintf(buf, buf_size, "Log file is empty.");
        return 0;
    }

    {
        int start = (total > cap) ? (total - cap) : 0;
        int n;
        for (n = total - 1; n >= start; n--)
        {
            const char *s = ring[n % cap];
            if (s != NULL && pos + 1U < buf_size)
            {
                size_t slen = strlen(s);
                if (pos + slen >= buf_size)
                {
                    slen = buf_size - pos - 1U;
                }
                rt_memcpy(buf + pos, s, slen);
                pos += slen;
                buf[pos] = '\0';
            }
        }
    }

    for (idx = 0; idx < cap; idx++)
    {
        if (ring[idx] != NULL)
        {
            rt_free(ring[idx]);
        }
    }

    return total;
}

int alert_clear_logs(void)
{
    FILE *fp;
    rt_base_t level;

    rt_mutex_take(&g_ctx.log_lock, RT_WAITING_FOREVER);
    fp = fopen(LOG_PATH_DEFAULT, "w");
    if (fp != NULL)
    {
        fclose(fp);
    }
    level = rt_hw_interrupt_disable();
    g_ctx.danger_count = 0U;
    g_ctx.live_session_alerts = 0U;
    rt_hw_interrupt_enable(level);
    rt_mutex_release(&g_ctx.log_lock);
    return (fp != NULL) ? RT_EOK : -RT_EIO;
}
