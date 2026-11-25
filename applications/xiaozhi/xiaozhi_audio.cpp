/*
 * XiaoZhi Audio Pipeline — PDM Mic (16 kHz mono -> Opus 60 ms) & ES8388 Speaker (Opus -> 24 kHz mono)
 *
 * Uses dynamic allocation on unified HyperRAM/SoCMEM heap to preserve DTCM,
 * and coordinates sound0 ownership with alert_manager via a shared audio arbiter.
 */

#include <rtthread.h>
#include <rtdevice.h>
#include <string.h>
#include <stdlib.h>
extern "C" {
#include <wavplayer.h>
#include "opus.h"
#include "xiaozhi.h"
#include "alert_manager.h"
}

#define DBG_TAG "xz.audio"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

#ifndef BSP_XIAOZHI_SOUND_DEVICE_NAME
#define BSP_XIAOZHI_SOUND_DEVICE_NAME "sound0"
#endif

#ifndef BSP_XIAOZHI_MIC_DEVICE_NAME
#define BSP_XIAOZHI_MIC_DEVICE_NAME "mic0"
#endif

/* 16 kHz mono, 60 ms = 960 samples = 1920 bytes */
#define XZ_MIC_SAMPLE_RATE      16000
#define XZ_MIC_CHANNELS         1
#define XZ_MIC_FRAME_SAMPLES    960
#define XZ_MIC_FRAME_BYTES      (XZ_MIC_FRAME_SAMPLES * XZ_MIC_CHANNELS * 2)
#define XZ_OPUS_MAX_PACKET      512

/* 24 kHz mono, 60 ms = 1440 samples = 2880 bytes */
#define XZ_SPK_SAMPLE_RATE      24000
#define XZ_SPK_CHANNELS         1
#define XZ_SPK_FRAME_SAMPLES    1440
#define XZ_SPK_FRAME_BYTES      (XZ_SPK_FRAME_SAMPLES * XZ_SPK_CHANNELS * 2)

#define XZ_DOWNLINK_QUEUE_NUM   32
#define XZ_EVENT_MIC_RX         (1U << 0)
#define XZ_EVENT_DOWNLINK       (1U << 1)
#define XZ_EVENT_LOCAL_VOICE    (1U << 2)
#define XZ_EVENT_ALL            (XZ_EVENT_MIC_RX | XZ_EVENT_DOWNLINK | XZ_EVENT_LOCAL_VOICE)

#define MIC_EVENT_OPEN          (1U << 0)
#define MIC_EVENT_CLOSE         (1U << 1)

#define GEMV_MAGIC              0x564D4547UL

extern "C" {
extern const unsigned char gemini_voices_bin[];
extern const size_t gemini_voices_bin_size;
}

typedef struct
{
    rt_slist_t node;
    uint8_t   *data;
    uint16_t   data_len;
    uint16_t   cap;
} xz_decode_node_t;

typedef struct
{
    rt_bool_t    inited;
    volatile rt_bool_t is_rx_enable;
    volatile rt_bool_t is_tx_enable;
    volatile rt_bool_t sound_opened;
    volatile rt_bool_t local_voice_playing;
    volatile rt_bool_t local_voice_abort;
    volatile rt_bool_t ext_stream_active;
    volatile rt_bool_t ext_stream_abort;
    volatile int       pending_voice_clip;
    volatile uint8_t   mic_level;
    volatile uint8_t   spk_level;

    rt_event_t   audio_evt;
    rt_event_t   mic_evt;
    rt_mutex_t   sound_lock;

    rt_device_t  rt_audio_dev;
    rt_device_t  rt_mic_dev;

    OpusEncoder *encoder;
    OpusDecoder *decoder;

    struct rt_ringbuffer *rb_mic_pcm;
    uint8_t      encode_in[XZ_MIC_FRAME_BYTES];
    uint8_t      encode_out[XZ_OPUS_MAX_PACKET];
    opus_int16   decode_out[XZ_SPK_FRAME_SAMPLES];

    xz_decode_node_t downlink_pool[XZ_DOWNLINK_QUEUE_NUM];
    rt_slist_t   downlink_busy;
    rt_slist_t   downlink_idle;

    rt_thread_t  opus_tid;
    rt_thread_t  mic_tid;
} xz_audio_ctx_t;

static xz_audio_ctx_t *g_aud = RT_NULL;
static struct rt_mutex g_arbiter_mutex;
static rt_bool_t g_arbiter_inited = RT_FALSE;

static void ensure_arbiter_init(void)
{
    if (!g_arbiter_inited)
    {
        rt_mutex_init(&g_arbiter_mutex, "aud_arb", RT_IPC_FLAG_PRIO);
        g_arbiter_inited = RT_TRUE;
    }
}

void xiaozhi_audio_lock(void)
{
    ensure_arbiter_init();
    rt_mutex_take(&g_arbiter_mutex, RT_WAITING_FOREVER);
}

void xiaozhi_audio_unlock(void)
{
    if (g_arbiter_inited)
    {
        rt_mutex_release(&g_arbiter_mutex);
    }
}

static uint8_t calc_pcm_level(const int16_t *samples, size_t count)
{
    uint32_t peak = 0U;
    for (size_t i = 0; i < count; i += 4U)
    {
        int32_t v = samples[i];
        uint32_t av = (v < 0) ? (uint32_t)(-v) : (uint32_t)v;
        if (av > peak) peak = av;
    }
    /* Map 0..16000 speech amplitude smoothly to 0..100 */
    uint32_t lvl = (peak * 100U) / 14000U;
    if (lvl > 100U) lvl = 100U;
    return (uint8_t)lvl;
}

static void flush_downlink_queue(xz_audio_ctx_t *ctx)
{
    rt_slist_t *node;
    rt_enter_critical();
    while ((node = rt_slist_first(&ctx->downlink_busy)) != RT_NULL)
    {
        rt_slist_remove(&ctx->downlink_busy, node);
        rt_slist_append(&ctx->downlink_idle, node);
    }
    rt_exit_critical();
}

static void speaker_hw_close_locked(xz_audio_ctx_t *ctx)
{
    if (ctx != RT_NULL && ctx->sound_opened && ctx->rt_audio_dev != RT_NULL)
    {
        rt_device_close(ctx->rt_audio_dev);
        ctx->sound_opened = RT_FALSE;
    }
    if (ctx != RT_NULL)
    {
        ctx->spk_level = 0U;
    }
}

static rt_err_t speaker_hw_open_locked(xz_audio_ctx_t *ctx)
{
    if (ctx == RT_NULL) return -RT_ERROR;
    if (ctx->sound_opened) return RT_EOK;

    if (ctx->rt_audio_dev == RT_NULL)
    {
        ctx->rt_audio_dev = rt_device_find(BSP_XIAOZHI_SOUND_DEVICE_NAME);
        if (ctx->rt_audio_dev == RT_NULL)
        {
            LOG_E("audio device '%s' not found", BSP_XIAOZHI_SOUND_DEVICE_NAME);
            return -RT_ERROR;
        }
    }

    /* Stop any ongoing wavplayer clip before opening sound0 for TTS */
    if (wavplayer_state_get() != PLAYER_STATE_STOPED)
    {
        wavplayer_stop();
        rt_thread_mdelay(40);
    }

    rt_err_t ret = rt_device_open(ctx->rt_audio_dev, RT_DEVICE_OFLAG_WRONLY);
    if (ret != RT_EOK)
    {
        LOG_E("open %s failed (%d)", BSP_XIAOZHI_SOUND_DEVICE_NAME, ret);
        return ret;
    }

    struct rt_audio_caps caps;
    rt_memset(&caps, 0, sizeof(caps));
    caps.main_type = AUDIO_TYPE_OUTPUT;
    caps.sub_type  = AUDIO_DSP_PARAM;
    caps.udata.config.channels   = XZ_SPK_CHANNELS;
    caps.udata.config.samplebits = 16;
    caps.udata.config.samplerate = XZ_SPK_SAMPLE_RATE;
    rt_device_control(ctx->rt_audio_dev, AUDIO_CTL_CONFIGURE, &caps);

    caps.main_type   = AUDIO_TYPE_MIXER;
    caps.sub_type    = AUDIO_MIXER_VOLUME;
    caps.udata.value = alert_get_volume();
    rt_device_control(ctx->rt_audio_dev, AUDIO_CTL_CONFIGURE, &caps);

    ctx->sound_opened = RT_TRUE;
    return RT_EOK;
}

void xiaozhi_audio_preempt_for_alarm(void)
{
    if (g_aud == RT_NULL) return;
    g_aud->local_voice_abort = RT_TRUE;
    g_aud->ext_stream_abort  = RT_TRUE;
    g_aud->pending_voice_clip = -1;
    g_aud->is_tx_enable = RT_FALSE;
    flush_downlink_queue(g_aud);
    xiaozhi_audio_lock();
    speaker_hw_close_locked(g_aud);
    xiaozhi_audio_unlock();
}

int xz_audio_stream_begin(void)
{
    if (g_aud == RT_NULL || !g_aud->inited || g_aud->decoder == RT_NULL)
        return -RT_ERROR;
    g_aud->local_voice_abort  = RT_TRUE;
    g_aud->pending_voice_clip = -1;
    g_aud->ext_stream_abort   = RT_FALSE;

    xiaozhi_audio_lock();
    rt_err_t ret = speaker_hw_open_locked(g_aud);
    if (ret == RT_EOK)
    {
        opus_decoder_ctl(g_aud->decoder, OPUS_RESET_STATE);
        g_aud->ext_stream_active = RT_TRUE;
    }
    xiaozhi_audio_unlock();
    return ret;
}

void xz_audio_stream_feed_opus(const uint8_t *pkt, uint16_t len)
{
    if (g_aud == RT_NULL || !g_aud->ext_stream_active || g_aud->ext_stream_abort ||
        pkt == RT_NULL || len == 0U || g_aud->decoder == RT_NULL)
        return;

    /* Decode directly on caller thread (xz_tts) while holding audio arbiter lock */
    xiaozhi_audio_lock();
    if (g_aud->ext_stream_active && !g_aud->ext_stream_abort &&
        g_aud->sound_opened && g_aud->rt_audio_dev != RT_NULL)
    {
        opus_int32 decoded = opus_decode(g_aud->decoder, pkt, (opus_int32)len,
                                         g_aud->decode_out, XZ_SPK_FRAME_SAMPLES, 0);
        if (decoded > 0)
        {
            g_aud->spk_level = calc_pcm_level(g_aud->decode_out, (size_t)decoded);
            rt_device_write(g_aud->rt_audio_dev, 0,
                            (const void *)g_aud->decode_out,
                            (rt_size_t)(decoded * 2));
        }
    }
    xiaozhi_audio_unlock();
}

void xz_audio_stream_end(void)
{
    if (g_aud == RT_NULL) return;
    g_aud->ext_stream_active = RT_FALSE;
    g_aud->spk_level = 0U;
    if (!g_aud->is_tx_enable)
    {
        xiaozhi_audio_lock();
        if (!g_aud->is_tx_enable)
            speaker_hw_close_locked(g_aud);
        xiaozhi_audio_unlock();
    }
}

void xz_audio_stream_abort(void)
{
    if (g_aud == RT_NULL) return;
    g_aud->ext_stream_abort  = RT_TRUE;
    g_aud->ext_stream_active = RT_FALSE;
    g_aud->spk_level = 0U;
}

void xz_audio_play_local_voice(xz_voice_clip_t clip)
{
    if (g_aud == RT_NULL || !g_aud->inited || clip >= XZ_VOICE_COUNT)
        return;
    /* Interrupt any ongoing local voice clip and queue the new one */
    g_aud->local_voice_abort = RT_TRUE;
    g_aud->pending_voice_clip = (int)clip;
    rt_event_send(g_aud->audio_evt, XZ_EVENT_LOCAL_VOICE);
}

void xz_audio_stop_local_voice(void)
{
    if (g_aud == RT_NULL) return;
    g_aud->local_voice_abort = RT_TRUE;
    g_aud->pending_voice_clip = -1;
}

rt_bool_t xz_audio_is_local_voice_playing(void)
{
    return (g_aud != RT_NULL && g_aud->local_voice_playing) ? RT_TRUE : RT_FALSE;
}

static void play_embedded_opus_clip(xz_audio_ctx_t *ctx, int clip_idx)
{
    if (ctx == RT_NULL || ctx->decoder == RT_NULL || clip_idx < 0)
        return;
    if (gemini_voices_bin_size < 16U)
        return;

    uint32_t magic = 0U, count = 0U;
    rt_memcpy(&magic, gemini_voices_bin, 4U);
    rt_memcpy(&count, gemini_voices_bin + 4U, 4U);
    if (magic != GEMV_MAGIC || (uint32_t)clip_idx >= count)
    {
        LOG_W("invalid voice pack (magic=0x%08lx count=%lu idx=%d)",
              (unsigned long)magic, (unsigned long)count, clip_idx);
        return;
    }

    uint32_t entry_off = 8U + (uint32_t)clip_idx * 8U;
    if (entry_off + 8U > gemini_voices_bin_size) return;

    uint32_t blob_off = 0U, blob_sz = 0U;
    rt_memcpy(&blob_off, gemini_voices_bin + entry_off, 4U);
    rt_memcpy(&blob_sz,  gemini_voices_bin + entry_off + 4U, 4U);
    if (blob_off + blob_sz > gemini_voices_bin_size) return;

    const uint8_t *p   = gemini_voices_bin + blob_off;
    const uint8_t *end = p + blob_sz;

    /* Reset Opus decoder state cleanly and open sound0 under audio arbiter lock */
    xiaozhi_audio_lock();
    opus_decoder_ctl(ctx->decoder, OPUS_RESET_STATE);
    rt_err_t open_ret = speaker_hw_open_locked(ctx);
    xiaozhi_audio_unlock();
    if (open_ret != RT_EOK) return;

    ctx->local_voice_playing = RT_TRUE;
    ctx->local_voice_abort   = RT_FALSE;
    LOG_I("Gemini-chan speaking local TTS clip #%d (%lu B)", clip_idx, (unsigned long)blob_sz);

    while ((p + 2U) <= end && !ctx->local_voice_abort && !ctx->is_tx_enable && !ctx->ext_stream_active)
    {
        uint16_t pkt_len = (uint16_t)p[0] | ((uint16_t)p[1] << 8);
        p += 2U;
        if (pkt_len == 0U || (p + pkt_len) > end)
            break;

        const uint8_t *frame_ptr = p;
        p += pkt_len;

        xiaozhi_audio_lock();
        if (!ctx->local_voice_abort && !ctx->is_tx_enable && !ctx->ext_stream_active &&
            ctx->sound_opened && ctx->rt_audio_dev != RT_NULL)
        {
            opus_int32 decoded = opus_decode(ctx->decoder, frame_ptr, (opus_int32)pkt_len,
                                             ctx->decode_out, XZ_SPK_FRAME_SAMPLES, 0);
            if (decoded > 0)
            {
                ctx->spk_level = calc_pcm_level(ctx->decode_out, (size_t)decoded);
                rt_device_write(ctx->rt_audio_dev, 0,
                                (const void *)ctx->decode_out,
                                (rt_size_t)(decoded * 2));
            }
        }
        xiaozhi_audio_unlock();
    }

    ctx->local_voice_playing = RT_FALSE;
    ctx->spk_level = 0U;

    /* Close sound0 cleanly if neither cloud TTS nor LAN Xiaoyi TTS took over */
    if (!ctx->is_tx_enable && !ctx->ext_stream_active)
    {
        xiaozhi_audio_lock();
        if (!ctx->is_tx_enable && !ctx->ext_stream_active)
        {
            speaker_hw_close_locked(ctx);
        }
        xiaozhi_audio_unlock();
        xz_on_local_voice_finished();
    }
}

void xz_play_wake_sound(void)
{
    xz_audio_stop_local_voice();
    xiaozhi_audio_lock();
    if (g_aud != RT_NULL && g_aud->sound_opened)
    {
        speaker_hw_close_locked(g_aud);
    }
    wavplayer_volume_set(alert_get_volume());
    if (wavplayer_play((char *)"/xiaozhi_ding.wav") == RT_EOK)
    {
        /* Let the short wake chime finish before opening mic0 so mic won't capture it */
        for (int i = 0; i < 45; i++)
        {
            rt_thread_mdelay(25);
            if (wavplayer_state_get() == PLAYER_STATE_STOPED)
                break;
        }
    }
    xiaozhi_audio_unlock();
}

void xz_mic(int on)
{
    if (g_aud == RT_NULL || !g_aud->inited || g_aud->rt_mic_dev == RT_NULL)
    {
        return;
    }

    if (on)
    {
        xz_audio_stop_local_voice();
        if (!g_aud->is_rx_enable)
        {
            rt_ringbuffer_reset(g_aud->rb_mic_pcm);
            if (rt_device_open(g_aud->rt_mic_dev, RT_DEVICE_OFLAG_RDONLY) == RT_EOK)
            {
                g_aud->is_rx_enable = RT_TRUE;
                rt_event_send(g_aud->mic_evt, MIC_EVENT_OPEN);
                LOG_I("mic0 opened (16kHz mono)");
            }
            else
            {
                LOG_E("failed to open mic0");
            }
        }
    }
    else
    {
        if (g_aud->is_rx_enable)
        {
            g_aud->is_rx_enable = RT_FALSE;
            g_aud->mic_level = 0U;
            rt_event_send(g_aud->mic_evt, MIC_EVENT_CLOSE);
            rt_device_close(g_aud->rt_mic_dev);
            LOG_I("mic0 closed");
        }
    }
}

int xz_mic_is_enabled(void)
{
    return (g_aud != RT_NULL && g_aud->is_rx_enable) ? 1 : 0;
}

void xz_speaker(int on)
{
    if (g_aud == RT_NULL || !g_aud->inited)
    {
        return;
    }

    if (on)
    {
        g_aud->local_voice_abort = RT_TRUE;
        g_aud->pending_voice_clip = -1;
        xiaozhi_audio_lock();
        if (speaker_hw_open_locked(g_aud) == RT_EOK)
        {
            g_aud->is_tx_enable = RT_TRUE;
        }
        xiaozhi_audio_unlock();
    }
    else
    {
        g_aud->is_tx_enable = RT_FALSE;
        flush_downlink_queue(g_aud);
        xiaozhi_audio_lock();
        speaker_hw_close_locked(g_aud);
        xiaozhi_audio_unlock();
    }
}

int xz_speaker_is_enabled(void)
{
    return (g_aud != RT_NULL && g_aud->is_tx_enable) ? 1 : 0;
}

uint8_t xz_audio_get_mic_level(void)
{
    return (g_aud != RT_NULL) ? g_aud->mic_level : 0U;
}

uint8_t xz_audio_get_spk_level(void)
{
    return (g_aud != RT_NULL) ? g_aud->spk_level : 0U;
}

static void mic_thread_entry(void *param)
{
    (void)param;
    rt_uint32_t evt = 0U;
    uint8_t chunk[640];

    while (1)
    {
        if (rt_event_recv(g_aud->mic_evt, MIC_EVENT_OPEN,
                          RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR,
                          RT_WAITING_FOREVER, &evt) != RT_EOK)
        {
            continue;
        }

        while (g_aud->is_rx_enable)
        {
            rt_uint32_t stop_evt = 0U;
            if (rt_event_recv(g_aud->mic_evt, MIC_EVENT_CLOSE,
                              RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR,
                              RT_WAITING_NO, &stop_evt) == RT_EOK)
            {
                if (stop_evt & MIC_EVENT_CLOSE) break;
            }

            rt_ssize_t n = rt_device_read(g_aud->rt_mic_dev, 0, chunk, sizeof(chunk));
            if (n > 0)
            {
                rt_ringbuffer_put(g_aud->rb_mic_pcm, chunk, (rt_uint16_t)n);
                if (rt_ringbuffer_data_len(g_aud->rb_mic_pcm) >= XZ_MIC_FRAME_BYTES)
                {
                    rt_event_send(g_aud->audio_evt, XZ_EVENT_MIC_RX);
                }
            }
            else
            {
                rt_thread_mdelay(2);
            }
        }
        g_aud->mic_level = 0U;
    }
}

static void xz_opus_thread_entry(void *param)
{
    (void)param;
    int err = 0;

    g_aud->decoder = opus_decoder_create(XZ_SPK_SAMPLE_RATE, XZ_SPK_CHANNELS, &err);
    if (g_aud->decoder == RT_NULL || err != OPUS_OK)
    {
        LOG_E("opus_decoder_create failed (%d)", err);
        return;
    }

    g_aud->encoder = opus_encoder_create(XZ_MIC_SAMPLE_RATE, XZ_MIC_CHANNELS,
                                         OPUS_APPLICATION_VOIP, &err);
    if (g_aud->encoder == RT_NULL || err != OPUS_OK)
    {
        LOG_E("opus_encoder_create failed (%d)", err);
        opus_decoder_destroy(g_aud->decoder);
        g_aud->decoder = RT_NULL;
        return;
    }

    opus_encoder_ctl(g_aud->encoder, OPUS_SET_EXPERT_FRAME_DURATION(OPUS_FRAMESIZE_60_MS));
    opus_encoder_ctl(g_aud->encoder, OPUS_SET_VBR(1));
    opus_encoder_ctl(g_aud->encoder, OPUS_SET_VBR_CONSTRAINT(1));
    opus_encoder_ctl(g_aud->encoder, OPUS_SET_BITRATE(16000));
    opus_encoder_ctl(g_aud->encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    opus_encoder_ctl(g_aud->encoder, OPUS_SET_COMPLEXITY(0));
    opus_encoder_ctl(g_aud->encoder, OPUS_SET_LSB_DEPTH(16));
    opus_encoder_ctl(g_aud->encoder, OPUS_SET_DTX(0));
    opus_encoder_ctl(g_aud->encoder, OPUS_SET_INBAND_FEC(0));
    opus_encoder_ctl(g_aud->encoder, OPUS_SET_MAX_BANDWIDTH(OPUS_BANDWIDTH_WIDEBAND));

    LOG_I("Opus codec ready (enc=16kHz/1ch/60ms, dec=24kHz/1ch)");

    while (1)
    {
        rt_uint32_t evt = 0U;
        rt_event_recv(g_aud->audio_evt, XZ_EVENT_ALL,
                      RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR,
                      RT_WAITING_FOREVER, &evt);

        /* Uplink: encode 60 ms mic PCM frames */
        while (g_aud->is_rx_enable &&
               rt_ringbuffer_data_len(g_aud->rb_mic_pcm) >= XZ_MIC_FRAME_BYTES)
        {
            rt_ringbuffer_get(g_aud->rb_mic_pcm, g_aud->encode_in, XZ_MIC_FRAME_BYTES);
            g_aud->mic_level = calc_pcm_level((const int16_t *)g_aud->encode_in,
                                              XZ_MIC_FRAME_SAMPLES);

            opus_int32 out_len = opus_encode(g_aud->encoder,
                                             (const opus_int16 *)g_aud->encode_in,
                                             XZ_MIC_FRAME_SAMPLES,
                                             g_aud->encode_out,
                                             sizeof(g_aud->encode_out));
            if (out_len > 0)
            {
                xz_audio_send_using_websocket(g_aud->encode_out, (int)out_len);
            }
            else
            {
                LOG_W("opus_encode err=%d", (int)out_len);
            }
        }

        /* Downlink: decode queued Opus TTS packets and write to sound0 */
        while (g_aud->is_tx_enable)
        {
            rt_slist_t *node = RT_NULL;
            rt_enter_critical();
            node = rt_slist_first(&g_aud->downlink_busy);
            if (node != RT_NULL)
            {
                rt_slist_remove(&g_aud->downlink_busy, node);
            }
            rt_exit_critical();

            if (node == RT_NULL)
            {
                g_aud->spk_level = 0U;
                break;
            }

            xz_decode_node_t *pkt = rt_container_of(node, xz_decode_node_t, node);
            opus_int32 decoded = 0;

            xiaozhi_audio_lock();
            if (g_aud->is_tx_enable && !g_aud->ext_stream_active &&
                g_aud->sound_opened && g_aud->rt_audio_dev != RT_NULL)
            {
                decoded = opus_decode(g_aud->decoder,
                                      pkt->data, pkt->data_len,
                                      g_aud->decode_out,
                                      XZ_SPK_FRAME_SAMPLES, 0);
                if (decoded > 0)
                {
                    g_aud->spk_level = calc_pcm_level(g_aud->decode_out, (size_t)decoded);
                    rt_device_write(g_aud->rt_audio_dev, 0,
                                    (const void *)g_aud->decode_out,
                                    (rt_size_t)(decoded * 2));
                }
            }
            xiaozhi_audio_unlock();

            rt_enter_critical();
            rt_slist_append(&g_aud->downlink_idle, node);
            rt_exit_critical();

            if (decoded < 0)
            {
                LOG_W("opus_decode err=%d", (int)decoded);
            }
        }

        /* Local embedded cute female TTS playback (runs whenever cloud TTS/mic is idle) */
        while (g_aud->pending_voice_clip >= 0 && !g_aud->is_tx_enable && !g_aud->is_rx_enable)
        {
            int clip = g_aud->pending_voice_clip;
            g_aud->pending_voice_clip = -1;
            play_embedded_opus_clip(g_aud, clip);
        }
    }
}

void xz_audio_downlink(const uint8_t *data, uint32_t size)
{
    if (g_aud == RT_NULL || !g_aud->inited || data == RT_NULL || size == 0U)
    {
        return;
    }
    if (!g_aud->is_tx_enable)
    {
        return;
    }

    /* Non-blocking pop: called from tcpip_thread WS_DATA callback, never sleep here */
    rt_slist_t *idle = RT_NULL;
    rt_enter_critical();
    idle = rt_slist_first(&g_aud->downlink_idle);
    if (idle != RT_NULL)
    {
        rt_slist_remove(&g_aud->downlink_idle, idle);
    }
    rt_exit_critical();

    if (idle == RT_NULL)
    {
        return;
    }

    xz_decode_node_t *pkt = rt_container_of(idle, xz_decode_node_t, node);
    if (pkt->cap < size + 16U)
    {
        if (pkt->data != RT_NULL) rt_free(pkt->data);
        pkt->cap = (uint16_t)(size + 16U);
        pkt->data = (uint8_t *)rt_malloc(pkt->cap);
        if (pkt->data == RT_NULL)
        {
            pkt->cap = 0U;
            rt_enter_critical();
            rt_slist_append(&g_aud->downlink_idle, idle);
            rt_exit_critical();
            return;
        }
    }

    pkt->data_len = (uint16_t)size;
    rt_memcpy(pkt->data, data, size);

    rt_enter_critical();
    rt_slist_append(&g_aud->downlink_busy, idle);
    rt_exit_critical();
    rt_event_send(g_aud->audio_evt, XZ_EVENT_DOWNLINK);
}

void xz_audio_init_system(void)
{
    ensure_arbiter_init();

    if (g_aud != RT_NULL && g_aud->inited)
    {
        return;
    }

    g_aud = (xz_audio_ctx_t *)rt_calloc(1, sizeof(xz_audio_ctx_t));
    if (g_aud == RT_NULL)
    {
        LOG_E("alloc xz_audio_ctx_t failed");
        return;
    }

    g_aud->audio_evt = rt_event_create("xzaudio", RT_IPC_FLAG_FIFO);
    g_aud->mic_evt   = rt_event_create("xzmic",   RT_IPC_FLAG_FIFO);
    g_aud->rb_mic_pcm = rt_ringbuffer_create(XZ_MIC_FRAME_BYTES * 3);
    g_aud->pending_voice_clip = -1;

    rt_slist_init(&g_aud->downlink_idle);
    rt_slist_init(&g_aud->downlink_busy);
    for (int i = 0; i < XZ_DOWNLINK_QUEUE_NUM; i++)
    {
        g_aud->downlink_pool[i].cap  = 256U;
        g_aud->downlink_pool[i].data = (uint8_t *)rt_malloc(256U);
        if (g_aud->downlink_pool[i].data != RT_NULL)
        {
            rt_slist_append(&g_aud->downlink_idle, &g_aud->downlink_pool[i].node);
        }
    }

    g_aud->rt_audio_dev = rt_device_find(BSP_XIAOZHI_SOUND_DEVICE_NAME);
    g_aud->rt_mic_dev   = rt_device_find(BSP_XIAOZHI_MIC_DEVICE_NAME);
    if (g_aud->rt_mic_dev != RT_NULL)
    {
        struct rt_audio_caps mic_caps;
        rt_memset(&mic_caps, 0, sizeof(mic_caps));
        mic_caps.main_type   = AUDIO_TYPE_MIXER;
        mic_caps.sub_type    = AUDIO_MIXER_VOLUME;
        mic_caps.udata.value = 38; /* balanced PDM gain without clipping */
        rt_device_control(g_aud->rt_mic_dev, AUDIO_CTL_CONFIGURE, &mic_caps);
    }
    else
    {
        LOG_W("mic device '%s' not found", BSP_XIAOZHI_MIC_DEVICE_NAME);
    }

    g_aud->opus_tid = rt_thread_create("xz_opus", xz_opus_thread_entry, RT_NULL,
                                       32 * 1024, 8, 10);
    g_aud->mic_tid  = rt_thread_create("xz_mic",  mic_thread_entry, RT_NULL,
                                       3 * 1024,  7, 10);

    if (g_aud->opus_tid != RT_NULL) rt_thread_startup(g_aud->opus_tid);
    if (g_aud->mic_tid != RT_NULL)  rt_thread_startup(g_aud->mic_tid);

    g_aud->inited = RT_TRUE;
    LOG_I("XiaoZhi audio pipeline initialized (mic=%s, spk=%s)",
          g_aud->rt_mic_dev ? "OK" : "MISSING",
          g_aud->rt_audio_dev ? "OK" : "MISSING");
}
