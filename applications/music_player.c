/*
 * TF Card Music Player — WAV + MP3 (minimp3) playback over sound0 (ES8388 I2S)
 *
 * Features:
 *   - Scans /sdcard/music (and /sdcard root, excluding system alarm/ding WAVs)
 *   - Supports UTF-8 Chinese filenames (FatFs FF_LFN_UNICODE=2)
 *   - Sequential, Loop All, Loop Single, and Fisher-Yates Shuffle modes
 *   - Seek, Pause/Resume (saves exact offset & releases sound0), Next/Prev, Stop
 *   - Cooperates with alert_manager and XiaoZhi TTS via music_player_preempt()
 */

#include <rtthread.h>
#include <rtdevice.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wavplayer.h>
#include "music_player.h"
#include "alert_manager.h"
#include "xiaozhi.h"
#include "sd_assets.h"
#include "minimp3.h"

#define DBG_TAG "music"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

#define MP_SOUND_DEV_NAME    PKG_WP_PLAY_DEVICE
#define MP_THREAD_STACK_SIZE 32768U
#define MP_THREAD_PRIO       10U
#define MP_MP3_INBUF_SIZE    16384U
#define MP_WAV_CHUNK_FRAMES  512U

enum
{
    MP_CMD_NONE = 0,
    MP_CMD_PLAY_IDX,
    MP_CMD_PAUSE,
    MP_CMD_RESUME,
    MP_CMD_STOP,
    MP_CMD_NEXT,
    MP_CMD_PREV,
    MP_CMD_SEEK,
    MP_CMD_PREEMPT,
};

typedef struct
{
    uint8_t  type;
    int32_t  arg;
} mp_cmd_t;

typedef struct
{
    rt_bool_t          inited;
    struct rt_mutex    lock;
    struct rt_mutex    preempt_lock;
    rt_mq_t            cmd_mq;
    rt_thread_t        worker;
    struct rt_completion preempt_ack;
    volatile rt_bool_t waiting_preempt;
    volatile rt_bool_t worker_active;

    volatile rt_bool_t abort_cur;
    volatile rt_bool_t seek_pending;
    volatile uint16_t  seek_permille_req;
    volatile rt_bool_t seek_ms_pending;
    volatile uint32_t  seek_ms_req;
    volatile uint8_t   pending_cmd;
    volatile int32_t   pending_arg;

    music_track_info_t tracks[MUSIC_MAX_TRACKS];
    uint32_t           track_count;
    uint8_t            shuffle_order[MUSIC_MAX_TRACKS];
    uint32_t           shuffle_pos;

    music_state_t      state;
    music_play_mode_t  mode;
    int                current_index;
    uint32_t           sample_rate;
    uint16_t           channels;
    uint32_t           elapsed_ms;
    uint32_t           duration_sec;
    uint16_t           progress_permille;
    uint32_t           library_rev;
    uint32_t           rev;

    /* Pause/resume position state */
    uint32_t           resume_offset;
    uint64_t           resume_samples;
    rt_bool_t          has_resume_pos;
    uint32_t           last_sd_gen;

    rt_device_t        sound_dev;
    rt_bool_t          sound_opened;
} mp_ctx_t;

static mp_ctx_t g_mp __attribute__((section(".cy_socmem_data")));

/* ---------------- Helpers ---------------- */

static rt_bool_t is_ignored_system_wav(const char *name)
{
    return (strcasecmp(name, "cockpit_alarm.wav") == 0 ||
            strcasecmp(name, "xiaozhi_ding.wav") == 0) ? RT_TRUE : RT_FALSE;
}

static music_format_t detect_format(const char *name)
{
    size_t len = strlen(name);
    if (len >= 4U)
    {
        const char *ext = name + len - 4U;
        if (strcasecmp(ext, ".mp3") == 0) return MUSIC_FMT_MP3;
        if (strcasecmp(ext, ".wav") == 0) return MUSIC_FMT_WAV;
    }
    return MUSIC_FMT_NONE;
}

static void make_track_title(const char *filename, char *out, size_t out_sz)
{
    size_t len = strlen(filename) - 4U; /* Scan accepts only .mp3 and .wav. */
    if (len >= out_sz) len = out_sz - 1U;
    while (len && ((unsigned char)filename[len] & 0xc0U) == 0x80U) len--;
    memcpy(out, filename, len);
    out[len] = '\0';
}

static int track_cmp(const void *a, const void *b)
{
    const music_track_info_t *ta = (const music_track_info_t *)a;
    const music_track_info_t *tb = (const music_track_info_t *)b;
    return strcasecmp(ta->title, tb->title);
}

static void rebuild_shuffle_locked(int keep_first)
{
    uint32_t n = g_mp.track_count;
    if (n == 0U)
    {
        g_mp.shuffle_pos = 0U;
        return;
    }
    for (uint32_t i = 0U; i < n; i++)
    {
        g_mp.shuffle_order[i] = (uint8_t)i;
    }
    uint32_t seed = (uint32_t)rt_tick_get() ^ (uint32_t)(uintptr_t)&g_mp;
    for (uint32_t i = n - 1U; i > 0U; i--)
    {
        seed = seed * 1664525UL + 1013904223UL;
        uint32_t j = (seed >> 16) % (i + 1U);
        uint8_t tmp = g_mp.shuffle_order[i];
        g_mp.shuffle_order[i] = g_mp.shuffle_order[j];
        g_mp.shuffle_order[j] = tmp;
    }
    if (keep_first >= 0 && n > 1U && (int)g_mp.shuffle_order[0] == keep_first)
    {
        uint8_t tmp = g_mp.shuffle_order[0];
        g_mp.shuffle_order[0] = g_mp.shuffle_order[n - 1U];
        g_mp.shuffle_order[n - 1U] = tmp;
    }
    g_mp.shuffle_pos = 0U;
}

static void scan_dir_locked(const char *dirpath)
{
    DIR *dir = opendir(dirpath);
    if (dir == RT_NULL)
    {
        return;
    }

    struct dirent *ent;
    char full[MUSIC_PATH_MAX];
    struct stat st;

    while (g_mp.track_count < MUSIC_MAX_TRACKS && (ent = readdir(dir)) != RT_NULL)
    {
        if (ent->d_name[0] == '.') continue;
        music_format_t fmt = detect_format(ent->d_name);
        if (fmt == MUSIC_FMT_NONE) continue;
        if (is_ignored_system_wav(ent->d_name)) continue;

        int length = snprintf(full, sizeof(full), "%s/%s", dirpath, ent->d_name);
        if (length <= 0 || (size_t)length >= sizeof(full)) continue;
        if (stat(full, &st) != 0 || S_ISDIR(st.st_mode) || st.st_size < 128)
        {
            continue;
        }

        /* Avoid duplicates if same path already added */
        rt_bool_t dup = RT_FALSE;
        for (uint32_t i = 0U; i < g_mp.track_count; i++)
        {
            if (strcmp(g_mp.tracks[i].path, full) == 0)
            {
                dup = RT_TRUE;
                break;
            }
        }
        if (dup) continue;

        music_track_info_t *tr = &g_mp.tracks[g_mp.track_count++];
        rt_memset(tr, 0, sizeof(*tr));
        memcpy(tr->path, full, (size_t)length + 1U);
        make_track_title(ent->d_name, tr->title, sizeof(tr->title));
        tr->format = fmt;
        tr->file_size = (uint32_t)st.st_size;
    }

    closedir(dir);
}

/* ---------------- Sound Device Management ---------------- */

static void sound_close_locked_hw(void)
{
    xiaozhi_audio_lock();
    if (g_mp.sound_opened && g_mp.sound_dev != RT_NULL)
    {
        rt_device_close(g_mp.sound_dev);
        g_mp.sound_opened = RT_FALSE;
    }
    xiaozhi_audio_unlock();
}

static rt_err_t sound_open_and_config(uint32_t samplerate)
{
    struct rt_audio_caps caps;

    if (!g_mp.sound_opened)
    {
        /* Preempt XiaoZhi & wavplayer before taking audio arbiter mutex */
        xiaozhi_audio_preempt_for_alarm();
        if (wavplayer_state_get() != PLAYER_STATE_STOPED)
        {
            (void)wavplayer_stop();
        }
    }

    xiaozhi_audio_lock();
    if (g_mp.abort_cur)
    {
        xiaozhi_audio_unlock();
        return -RT_EINTR;
    }

    if (!g_mp.sound_opened)
    {
        if (g_mp.sound_dev == RT_NULL)
        {
            g_mp.sound_dev = rt_device_find(MP_SOUND_DEV_NAME);
            if (g_mp.sound_dev == RT_NULL)
            {
                xiaozhi_audio_unlock();
                LOG_E("sound device '%s' not found", MP_SOUND_DEV_NAME);
                return -RT_ERROR;
            }
        }

        rt_err_t ret = rt_device_open(g_mp.sound_dev, RT_DEVICE_OFLAG_WRONLY);
        if (ret != RT_EOK)
        {
            xiaozhi_audio_unlock();
            LOG_E("open %s failed (%d)", MP_SOUND_DEV_NAME, ret);
            return ret;
        }
        g_mp.sound_opened = RT_TRUE;
    }

    rt_memset(&caps, 0, sizeof(caps));
    caps.main_type = AUDIO_TYPE_OUTPUT;
    caps.sub_type  = AUDIO_DSP_PARAM;
    caps.udata.config.channels   = 1; /* drv_i2s duplicates mono to L+R cleanly */
    caps.udata.config.samplebits = 16;
    caps.udata.config.samplerate = samplerate;
    (void)rt_device_control(g_mp.sound_dev, AUDIO_CTL_CONFIGURE, &caps);

    rt_memset(&caps, 0, sizeof(caps));
    caps.main_type   = AUDIO_TYPE_MIXER;
    caps.sub_type    = AUDIO_MIXER_VOLUME;
    caps.udata.value = alert_get_volume();
    (void)rt_device_control(g_mp.sound_dev, AUDIO_CTL_CONFIGURE, &caps);
    xiaozhi_audio_unlock();

    return RT_EOK;
}

static int pick_next_index_locked(rt_bool_t from_eof)
{
    if (g_mp.track_count == 0U) return -1;
    int n = (int)g_mp.track_count;
    int cur = g_mp.current_index;
    if (cur < 0 || cur >= n) cur = 0;

    if (from_eof && g_mp.mode == MUSIC_MODE_LOOP_ONE)
    {
        return cur;
    }

    if (g_mp.mode == MUSIC_MODE_SHUFFLE)
    {
        if (n == 1) return from_eof ? -1 : 0;
        if (g_mp.shuffle_pos + 1U >= (uint32_t)n)
        {
            rebuild_shuffle_locked(cur);
            return (int)g_mp.shuffle_order[0];
        }
        g_mp.shuffle_pos++;
        int next = (int)g_mp.shuffle_order[g_mp.shuffle_pos];
        if (next == cur)
        {
            g_mp.shuffle_pos = (g_mp.shuffle_pos + 1U) % (uint32_t)n;
            next = (int)g_mp.shuffle_order[g_mp.shuffle_pos];
        }
        return next;
    }

    if (from_eof && g_mp.mode == MUSIC_MODE_SEQ)
    {
        return (cur + 1 < n) ? (cur + 1) : -1;
    }

    return (cur + 1) % n;
}

static int pick_prev_index_locked(void)
{
    if (g_mp.track_count == 0U) return -1;
    int n = (int)g_mp.track_count;
    int cur = g_mp.current_index;
    if (cur < 0 || cur >= n) return 0;

    if (g_mp.mode == MUSIC_MODE_SHUFFLE && n > 1)
    {
        if (g_mp.shuffle_pos > 0U) g_mp.shuffle_pos--;
        else g_mp.shuffle_pos = (uint32_t)(n - 1);
        return (int)g_mp.shuffle_order[g_mp.shuffle_pos];
    }

    return (cur + n - 1) % n;
}

/* ---------------- WAV Parser & Player ---------------- */

static uint16_t rd_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t rd_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static rt_bool_t parse_wav_chunks(FILE *fp, uint32_t *sr_out, uint16_t *ch_out,
                                  uint16_t *bits_out, uint32_t *data_off_out, uint32_t *data_sz_out)
{
    uint8_t hdr[12];
    uint8_t chdr[8];
    uint8_t fmt[16];
    rt_bool_t got_fmt = RT_FALSE;
    rt_bool_t got_data = RT_FALSE;

    if (fread(hdr, 1U, 12U, fp) != 12U) return RT_FALSE;
    if (memcmp(hdr, "RIFF", 4U) != 0 || memcmp(hdr + 8U, "WAVE", 4U) != 0)
    {
        return RT_FALSE;
    }

    while (fread(chdr, 1U, 8U, fp) == 8U)
    {
        uint32_t csz = rd_le32(chdr + 4U);
        if (memcmp(chdr, "fmt ", 4U) == 0)
        {
            if (csz < 16U || fread(fmt, 1U, 16U, fp) != 16U) return RT_FALSE;
            uint16_t afmt = rd_le16(fmt + 0U);
            uint16_t ch   = rd_le16(fmt + 2U);
            uint32_t sr   = rd_le32(fmt + 4U);
            uint16_t bits = rd_le16(fmt + 14U);
            if (afmt != 1U || (ch != 1U && ch != 2U) || (bits != 8U && bits != 16U) ||
                sr < 8000U || sr > 96000U)
            {
                return RT_FALSE;
            }
            *sr_out   = sr;
            *ch_out   = ch;
            *bits_out = bits;
            got_fmt = RT_TRUE;
            if (csz > 16U)
            {
                uint32_t skip = csz - 16U + (csz & 1U);
                if (fseek(fp, (long)skip, SEEK_CUR) != 0) return RT_FALSE;
            }
        }
        else if (memcmp(chdr, "data", 4U) == 0)
        {
            long pos = ftell(fp);
            if (pos < 0) return RT_FALSE;
            *data_off_out = (uint32_t)pos;
            *data_sz_out  = csz;
            got_data = RT_TRUE;
            break;
        }
        else
        {
            uint32_t skip = csz + (csz & 1U);
            if (fseek(fp, (long)skip, SEEK_CUR) != 0) break;
        }
    }

    return (got_fmt && got_data && *data_sz_out > 0U) ? RT_TRUE : RT_FALSE;
}

/* Returns: 0 = EOF normally, 1 = interrupted by command/preempt, -1 = error */
static int play_wav_track(const music_track_info_t *tr, rt_bool_t resume_from_saved)
{
    FILE *fp = fopen(tr->path, "rb");
    if (fp == NULL)
    {
        LOG_W("open wav failed: %s", tr->path);
        return -1;
    }

    uint32_t sr = 0U, data_off = 0U, data_sz = 0U;
    uint16_t ch = 0U, bits = 0U;
    if (!parse_wav_chunks(fp, &sr, &ch, &bits, &data_off, &data_sz))
    {
        LOG_W("unsupported wav format: %s", tr->path);
        fclose(fp);
        return -1;
    }

    uint32_t frame_bytes = (uint32_t)ch * (uint32_t)(bits / 8U);
    uint32_t bytes_per_sec = sr * frame_bytes;
    uint32_t dur_sec = (bytes_per_sec > 0U) ? (data_sz / bytes_per_sec) : 0U;

    uint32_t start_off = 0U;
    if (g_mp.seek_ms_pending)
    {
        uint32_t req_ms = g_mp.seek_ms_req;
        g_mp.seek_ms_pending = RT_FALSE;
        uint64_t target = (uint64_t)bytes_per_sec * req_ms / 1000ULL;
        if (target >= data_sz) target = (data_sz > frame_bytes) ? (data_sz - frame_bytes) : 0U;
        start_off = (uint32_t)target - ((uint32_t)target % frame_bytes);
    }
    else if (resume_from_saved && g_mp.has_resume_pos && g_mp.resume_offset < data_sz)
    {
        start_off = g_mp.resume_offset - (g_mp.resume_offset % frame_bytes);
    }
    if (fseek(fp, (long)(data_off + start_off), SEEK_SET) != 0)
    {
        fclose(fp);
        return -1;
    }

    if (sound_open_and_config(sr) != RT_EOK)
    {
        if (g_mp.abort_cur)
        {
            if (g_mp.pending_cmd == MP_CMD_PAUSE || g_mp.pending_cmd == MP_CMD_PREEMPT)
            {
                g_mp.resume_offset = start_off;
                g_mp.has_resume_pos = RT_TRUE;
            }
            fclose(fp);
            return 1;
        }
        fclose(fp);
        return -1;
    }

    uint8_t *io_buf   = (uint8_t *)rt_malloc(MP_WAV_CHUNK_FRAMES * 4U);
    int16_t *mono_buf = (int16_t *)rt_malloc(MP_WAV_CHUNK_FRAMES * sizeof(int16_t));
    if (io_buf == RT_NULL || mono_buf == RT_NULL)
    {
        if (io_buf) rt_free(io_buf);
        if (mono_buf) rt_free(mono_buf);
        sound_close_locked_hw();
        fclose(fp);
        return -1;
    }

    rt_mutex_take(&g_mp.lock, RT_WAITING_FOREVER);
    g_mp.state = MUSIC_STATE_PLAYING;
    g_mp.sample_rate = sr;
    g_mp.channels = ch;
    g_mp.duration_sec = dur_sec;
    g_mp.elapsed_ms = (uint32_t)((uint64_t)start_off * 1000U / bytes_per_sec);
    g_mp.progress_permille = (uint16_t)((uint64_t)start_off * 1000ULL / data_sz);
    g_mp.has_resume_pos = RT_FALSE;
    g_mp.rev++;
    rt_mutex_release(&g_mp.lock);

    uint32_t played_bytes = start_off;
    int result = 0;

    while (played_bytes < data_sz)
    {
        if (g_mp.seek_ms_pending)
        {
            uint32_t req_ms = g_mp.seek_ms_req;
            g_mp.seek_ms_pending = RT_FALSE;
            uint64_t target64 = (uint64_t)bytes_per_sec * req_ms / 1000ULL;
            if (target64 >= data_sz) target64 = (data_sz > frame_bytes) ? (data_sz - frame_bytes) : 0U;
            uint32_t target = (uint32_t)target64 - ((uint32_t)target64 % frame_bytes);
            (void)fseek(fp, (long)(data_off + target), SEEK_SET);
            played_bytes = target;
            rt_mutex_take(&g_mp.lock, RT_WAITING_FOREVER);
            g_mp.elapsed_ms = (uint32_t)((uint64_t)played_bytes * 1000U / bytes_per_sec);
            g_mp.progress_permille = (uint16_t)((uint64_t)played_bytes * 1000ULL / data_sz);
            g_mp.rev++;
            rt_mutex_release(&g_mp.lock);
            continue;
        }
        if (g_mp.seek_pending)
        {
            uint32_t pm = g_mp.seek_permille_req;
            g_mp.seek_pending = RT_FALSE;
            uint32_t target = (uint32_t)((uint64_t)data_sz * pm / 1000ULL);
            target -= (target % frame_bytes);
            if (target >= data_sz) target = (data_sz > frame_bytes) ? (data_sz - frame_bytes) : 0U;
            (void)fseek(fp, (long)(data_off + target), SEEK_SET);
            played_bytes = target;
            rt_mutex_take(&g_mp.lock, RT_WAITING_FOREVER);
            g_mp.elapsed_ms = (uint32_t)((uint64_t)played_bytes * 1000U / bytes_per_sec);
            g_mp.progress_permille = (uint16_t)((uint64_t)played_bytes * 1000ULL / data_sz);
            g_mp.rev++;
            rt_mutex_release(&g_mp.lock);
            continue;
        }
        if (g_mp.abort_cur)
        {
            if (g_mp.pending_cmd == MP_CMD_PAUSE || g_mp.pending_cmd == MP_CMD_PREEMPT)
            {
                g_mp.resume_offset = played_bytes;
                g_mp.has_resume_pos = RT_TRUE;
            }
            result = 1;
            break;
        }

        uint32_t remain = data_sz - played_bytes;
        uint32_t want_frames = MP_WAV_CHUNK_FRAMES;
        uint32_t want_bytes = want_frames * frame_bytes;
        if (want_bytes > remain)
        {
            want_bytes = remain - (remain % frame_bytes);
            if (want_bytes == 0U) break;
        }

        size_t got = fread(io_buf, 1U, want_bytes, fp);
        if (got < frame_bytes) break;
        uint32_t frames = (uint32_t)(got / frame_bytes);

        if (bits == 16U)
        {
            const int16_t *s16 = (const int16_t *)io_buf;
            if (ch == 1U)
            {
                rt_memcpy(mono_buf, s16, frames * sizeof(int16_t));
            }
            else
            {
                for (uint32_t i = 0U; i < frames; i++)
                {
                    int32_t mix = ((int32_t)s16[i * 2U] + (int32_t)s16[i * 2U + 1U]) >> 1;
                    mono_buf[i] = (int16_t)mix;
                }
            }
        }
        else
        {
            if (ch == 1U)
            {
                for (uint32_t i = 0U; i < frames; i++)
                {
                    mono_buf[i] = (int16_t)(((int32_t)io_buf[i] - 128) << 8);
                }
            }
            else
            {
                for (uint32_t i = 0U; i < frames; i++)
                {
                    int32_t l = ((int32_t)io_buf[i * 2U] - 128) << 8;
                    int32_t r = ((int32_t)io_buf[i * 2U + 1U] - 128) << 8;
                    mono_buf[i] = (int16_t)((l + r) >> 1);
                }
            }
        }

        rt_device_write(g_mp.sound_dev, 0, mono_buf, (rt_size_t)(frames * sizeof(int16_t)));
        played_bytes += frames * frame_bytes;

        uint32_t ms = (uint32_t)((uint64_t)played_bytes * 1000U / bytes_per_sec);
        uint16_t pm  = (uint16_t)((uint64_t)played_bytes * 1000ULL / data_sz);
        if (ms / 80U != g_mp.elapsed_ms / 80U || pm != g_mp.progress_permille)
        {
            rt_mutex_take(&g_mp.lock, RT_WAITING_FOREVER);
            g_mp.elapsed_ms = ms;
            g_mp.progress_permille = pm;
            g_mp.rev++;
            rt_mutex_release(&g_mp.lock);
        }
    }

    sound_close_locked_hw();
    rt_free(io_buf);
    rt_free(mono_buf);
    fclose(fp);
    return result;
}

/* ---------------- MP3 Decoder & Player ---------------- */

static uint32_t skip_id3v2_tag(FILE *fp)
{
    uint8_t hdr[10];
    if (fread(hdr, 1U, 10U, fp) != 10U)
    {
        (void)fseek(fp, 0, SEEK_SET);
        return 0U;
    }
    if (memcmp(hdr, "ID3", 3U) == 0)
    {
        uint32_t sz = ((uint32_t)(hdr[6] & 0x7FU) << 21) |
                      ((uint32_t)(hdr[7] & 0x7FU) << 14) |
                      ((uint32_t)(hdr[8] & 0x7FU) << 7)  |
                      (uint32_t)(hdr[9] & 0x7FU);
        uint32_t total = 10U + sz + ((hdr[5] & 0x10U) ? 10U : 0U);
        (void)fseek(fp, (long)total, SEEK_SET);
        return total;
    }
    (void)fseek(fp, 0, SEEK_SET);
    return 0U;
}

static int play_mp3_track(const music_track_info_t *tr, rt_bool_t resume_from_saved)
{
    FILE *fp = fopen(tr->path, "rb");
    if (fp == NULL)
    {
        LOG_W("open mp3 failed: %s", tr->path);
        return -1;
    }

    mp3dec_t *dec      = (mp3dec_t *)rt_malloc(sizeof(mp3dec_t));
    uint8_t  *inbuf    = (uint8_t *)rt_malloc(MP_MP3_INBUF_SIZE);
    int16_t  *pcm_buf  = (int16_t *)rt_malloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t));
    int16_t  *mono_buf = (int16_t *)rt_malloc(1152U * sizeof(int16_t));
    if (!dec || !inbuf || !pcm_buf || !mono_buf)
    {
        if (dec) rt_free(dec);
        if (inbuf) rt_free(inbuf);
        if (pcm_buf) rt_free(pcm_buf);
        if (mono_buf) rt_free(mono_buf);
        fclose(fp);
        return -1;
    }

    mp3dec_init(dec);
    uint32_t audio_start = skip_id3v2_tag(fp);
    uint32_t audio_bytes = (tr->file_size > audio_start) ? (tr->file_size - audio_start) : tr->file_size;
    if (audio_bytes == 0U) audio_bytes = 1U;

    uint32_t file_off = audio_start;
    uint64_t total_samples = 0ULL;
    if (resume_from_saved && g_mp.has_resume_pos &&
        g_mp.resume_offset >= audio_start && g_mp.resume_offset < tr->file_size)
    {
        file_off = g_mp.resume_offset;
        total_samples = g_mp.resume_samples;
        (void)fseek(fp, (long)file_off, SEEK_SET);
    }

    size_t in_len = 0U;
    rt_bool_t eof = RT_FALSE;
    uint32_t active_sr = 0U;
    uint32_t est_dur_sec = 0U;
    uint32_t est_dur_ms = 0U;
    int result = 0;

    rt_mutex_take(&g_mp.lock, RT_WAITING_FOREVER);
    g_mp.state = MUSIC_STATE_PLAYING;
    g_mp.has_resume_pos = RT_FALSE;
    g_mp.rev++;
    rt_mutex_release(&g_mp.lock);

    while (1)
    {
        if (g_mp.seek_ms_pending && active_sr > 0U && est_dur_ms > 0U)
        {
            uint32_t target_ms = g_mp.seek_ms_req;
            g_mp.seek_ms_pending = RT_FALSE;
            uint32_t max_ms = (est_dur_ms > 500U) ? (est_dur_ms - 500U) : 0U;
            if (target_ms > max_ms) target_ms = max_ms;
            file_off = audio_start + (uint32_t)((uint64_t)audio_bytes * target_ms / est_dur_ms);
            (void)fseek(fp, (long)file_off, SEEK_SET);
            mp3dec_init(dec);
            in_len = 0U;
            eof = RT_FALSE;
            total_samples = (uint64_t)target_ms * active_sr / 1000ULL;
            rt_mutex_take(&g_mp.lock, RT_WAITING_FOREVER);
            g_mp.elapsed_ms = target_ms;
            g_mp.progress_permille = (uint16_t)((uint64_t)target_ms * 1000ULL / est_dur_ms);
            g_mp.rev++;
            rt_mutex_release(&g_mp.lock);
            continue;
        }
        if (g_mp.seek_pending)
        {
            uint32_t pm = g_mp.seek_permille_req;
            g_mp.seek_pending = RT_FALSE;
            if (pm > 995U) pm = 995U;
            file_off = audio_start + (uint32_t)((uint64_t)audio_bytes * pm / 1000ULL);
            (void)fseek(fp, (long)file_off, SEEK_SET);
            mp3dec_init(dec);
            in_len = 0U;
            eof = RT_FALSE;
            if (est_dur_sec > 0U && active_sr > 0U)
            {
                total_samples = (uint64_t)est_dur_sec * pm * active_sr / 1000ULL;
            }
            rt_mutex_take(&g_mp.lock, RT_WAITING_FOREVER);
            g_mp.elapsed_ms = active_sr ? (uint32_t)(total_samples * 1000ULL / active_sr) : 0U;
            g_mp.progress_permille = (uint16_t)pm;
            g_mp.rev++;
            rt_mutex_release(&g_mp.lock);
            continue;
        }
        if (g_mp.abort_cur)
        {
            if (g_mp.pending_cmd == MP_CMD_PAUSE || g_mp.pending_cmd == MP_CMD_PREEMPT)
            {
                g_mp.resume_offset  = file_off;
                g_mp.resume_samples = total_samples;
                g_mp.has_resume_pos = RT_TRUE;
            }
            result = 1;
            break;
        }

        if (!eof && in_len < 4096U)
        {
            size_t space = MP_MP3_INBUF_SIZE - in_len;
            size_t got = fread(inbuf + in_len, 1U, space, fp);
            if (got == 0U)
            {
                eof = RT_TRUE;
            }
            else
            {
                in_len += got;
            }
        }

        if (in_len == 0U)
        {
            break;
        }

        mp3dec_frame_info_t info;
        int samples = mp3dec_decode_frame(dec, inbuf, (int)in_len, pcm_buf, &info);

        if (info.frame_bytes > 0)
        {
            if ((size_t)info.frame_bytes >= in_len)
            {
                in_len = 0U;
            }
            else
            {
                rt_memmove(inbuf, inbuf + info.frame_bytes, in_len - (size_t)info.frame_bytes);
                in_len -= (size_t)info.frame_bytes;
            }
            file_off += (uint32_t)info.frame_bytes;
        }
        else if (eof)
        {
            break;
        }
        else
        {
            /* Shift by 1 byte if decoder couldn't sync in buffered bytes */
            if (in_len > 1U)
            {
                rt_memmove(inbuf, inbuf + 1U, in_len - 1U);
                in_len--;
                file_off++;
            }
            else
            {
                in_len = 0U;
            }
            continue;
        }

        if (samples > 0 && info.hz > 0)
        {
            if ((uint32_t)info.hz != active_sr || !g_mp.sound_opened)
            {
                if (sound_open_and_config((uint32_t)info.hz) != RT_EOK)
                {
                    if (g_mp.abort_cur)
                    {
                        if (g_mp.pending_cmd == MP_CMD_PAUSE || g_mp.pending_cmd == MP_CMD_PREEMPT)
                        {
                            g_mp.resume_offset  = file_off;
                            g_mp.resume_samples = total_samples;
                            g_mp.has_resume_pos = RT_TRUE;
                        }
                        result = 1;
                    }
                    else
                    {
                        result = -1;
                    }
                    break;
                }
                active_sr = (uint32_t)info.hz;
                if (info.bitrate_kbps > 0)
                {
                    est_dur_ms = (uint32_t)(((uint64_t)audio_bytes * 8ULL) / (uint64_t)info.bitrate_kbps);
                    est_dur_sec = est_dur_ms / 1000U;
                }
                rt_mutex_take(&g_mp.lock, RT_WAITING_FOREVER);
                g_mp.sample_rate = active_sr;
                g_mp.channels = (uint16_t)info.channels;
                g_mp.duration_sec = est_dur_sec;
                g_mp.rev++;
                rt_mutex_release(&g_mp.lock);
            }

            if (g_mp.seek_ms_pending && active_sr > 0U && est_dur_ms > 0U)
            {
                continue;
            }

            if (info.channels == 2)
            {
                for (int i = 0; i < samples; i++)
                {
                    int32_t mix = ((int32_t)pcm_buf[i * 2] + (int32_t)pcm_buf[i * 2 + 1]) >> 1;
                    mono_buf[i] = (int16_t)mix;
                }
                rt_device_write(g_mp.sound_dev, 0, mono_buf, (rt_size_t)(samples * sizeof(int16_t)));
            }
            else
            {
                rt_device_write(g_mp.sound_dev, 0, pcm_buf, (rt_size_t)(samples * sizeof(int16_t)));
            }

            total_samples += (uint64_t)samples;
            uint32_t ms = (uint32_t)(total_samples * 1000ULL / active_sr);
            uint32_t rel = (file_off > audio_start) ? (file_off - audio_start) : 0U;
            uint16_t pm  = (uint16_t)((uint64_t)rel * 1000ULL / audio_bytes);
            if (pm > 1000U) pm = 1000U;

            if (ms / 80U != g_mp.elapsed_ms / 80U || (pm / 5U) != (g_mp.progress_permille / 5U))
            {
                rt_mutex_take(&g_mp.lock, RT_WAITING_FOREVER);
                g_mp.elapsed_ms = ms;
                g_mp.progress_permille = pm;
                if (est_dur_sec == 0U && ms >= 1000U && pm > 10U)
                {
                    g_mp.duration_sec = ms / pm;
                }
                g_mp.rev++;
                rt_mutex_release(&g_mp.lock);
            }
        }
    }

    sound_close_locked_hw();
    rt_free(mono_buf);
    rt_free(pcm_buf);
    rt_free(inbuf);
    rt_free(dec);
    fclose(fp);
    return result;
}

/* ---------------- Worker Thread ---------------- */

static void music_worker_entry(void *parameter)
{
    mp_cmd_t cmd;
    (void)parameter;

    (void)sdcard_wait_mounted(3000);
    if (sdcard_is_mounted())
    {
        (void)music_player_scan();
    }

    while (1)
    {
        if (rt_mq_recv(g_mp.cmd_mq, &cmd, sizeof(cmd), RT_WAITING_FOREVER) != sizeof(cmd))
        {
            continue;
        }

    process_cmd:
        if (cmd.type == MP_CMD_PAUSE || cmd.type == MP_CMD_PREEMPT)
        {
            rt_mutex_take(&g_mp.lock, RT_WAITING_FOREVER);
            if (g_mp.state == MUSIC_STATE_PLAYING)
            {
                g_mp.state = MUSIC_STATE_PAUSED;
                g_mp.rev++;
            }
            g_mp.worker_active = RT_FALSE;
            g_mp.abort_cur = RT_FALSE;
            g_mp.pending_cmd = MP_CMD_NONE;
            rt_mutex_release(&g_mp.lock);
            if (g_mp.waiting_preempt)
            {
                g_mp.waiting_preempt = RT_FALSE;
                rt_completion_done(&g_mp.preempt_ack);
            }
            continue;
        }
        if (cmd.type == MP_CMD_STOP)
        {
            rt_mutex_take(&g_mp.lock, RT_WAITING_FOREVER);
            g_mp.state = MUSIC_STATE_STOPPED;
            g_mp.elapsed_ms = 0U;
            g_mp.progress_permille = 0U;
            g_mp.has_resume_pos = RT_FALSE;
            g_mp.seek_ms_pending = RT_FALSE;
            g_mp.worker_active = RT_FALSE;
            g_mp.abort_cur = RT_FALSE;
            g_mp.pending_cmd = MP_CMD_NONE;
            g_mp.rev++;
            rt_mutex_release(&g_mp.lock);
            if (g_mp.waiting_preempt)
            {
                g_mp.waiting_preempt = RT_FALSE;
                rt_completion_done(&g_mp.preempt_ack);
            }
            continue;
        }

        /* If a newer control command already queued up, handle it before opening sound0 */
        {
            mp_cmd_t newer_cmd;
            if (rt_mq_recv(g_mp.cmd_mq, &newer_cmd, sizeof(newer_cmd), RT_WAITING_NO) == sizeof(newer_cmd))
            {
                cmd = newer_cmd;
                goto process_cmd;
            }
        }

        rt_bool_t resume_pos = RT_FALSE;
        int idx = -1;

        rt_mutex_take(&g_mp.lock, RT_WAITING_FOREVER);
        if (g_mp.track_count == 0U)
        {
            g_mp.state = MUSIC_STATE_STOPPED;
            g_mp.worker_active = RT_FALSE;
            g_mp.abort_cur = RT_FALSE;
            g_mp.pending_cmd = MP_CMD_NONE;
            g_mp.rev++;
            rt_mutex_release(&g_mp.lock);
            if (g_mp.waiting_preempt)
            {
                g_mp.waiting_preempt = RT_FALSE;
                rt_completion_done(&g_mp.preempt_ack);
            }
            continue;
        }

        if (cmd.type == MP_CMD_PLAY_IDX)
        {
            idx = (cmd.arg >= 0 && cmd.arg < (int)g_mp.track_count) ? (int)cmd.arg : 0;
            g_mp.has_resume_pos = RT_FALSE;
        }
        else if (cmd.type == MP_CMD_RESUME)
        {
            idx = (g_mp.current_index >= 0 && g_mp.current_index < (int)g_mp.track_count)
                      ? g_mp.current_index
                      : 0;
            resume_pos = g_mp.has_resume_pos;
        }
        else if (cmd.type == MP_CMD_NEXT)
        {
            idx = pick_next_index_locked(RT_FALSE);
            g_mp.has_resume_pos = RT_FALSE;
            g_mp.seek_ms_pending = RT_FALSE;
        }
        else if (cmd.type == MP_CMD_PREV)
        {
            idx = pick_prev_index_locked();
            g_mp.has_resume_pos = RT_FALSE;
            g_mp.seek_ms_pending = RT_FALSE;
        }

        if (idx < 0 || idx >= (int)g_mp.track_count)
        {
            g_mp.state = MUSIC_STATE_STOPPED;
            g_mp.worker_active = RT_FALSE;
            g_mp.abort_cur = RT_FALSE;
            g_mp.pending_cmd = MP_CMD_NONE;
            g_mp.rev++;
            rt_mutex_release(&g_mp.lock);
            if (g_mp.waiting_preempt)
            {
                g_mp.waiting_preempt = RT_FALSE;
                rt_completion_done(&g_mp.preempt_ack);
            }
            continue;
        }

        music_track_info_t tr = g_mp.tracks[idx];
        g_mp.current_index = idx;
        if (!resume_pos)
        {
            g_mp.elapsed_ms = 0U;
            g_mp.duration_sec = 0U;
            g_mp.progress_permille = 0U;
            g_mp.sample_rate = 0U;
            g_mp.channels = 0U;
        }
        g_mp.state = MUSIC_STATE_PLAYING;
        g_mp.worker_active = RT_TRUE;
        g_mp.abort_cur = RT_FALSE;
        g_mp.pending_cmd = MP_CMD_NONE;
        g_mp.rev++;
        rt_mutex_release(&g_mp.lock);

        LOG_I("play [%d/%lu] %s", idx + 1, (unsigned long)g_mp.track_count, tr.title);
        int play_ret = (tr.format == MUSIC_FMT_MP3)
                           ? play_mp3_track(&tr, resume_pos)
                           : play_wav_track(&tr, resume_pos);

        g_mp.worker_active = RT_FALSE;

        if (play_ret == 1 || g_mp.abort_cur)
        {
            g_mp.pending_cmd = MP_CMD_NONE;
            g_mp.abort_cur = RT_FALSE;
            /* Always dequeue the pending command from cmd_mq instead of bypassing it */
            continue;
        }

        /* Normal EOF or read error -> advance according to play mode */
        rt_mutex_take(&g_mp.lock, RT_WAITING_FOREVER);
        int next_idx = (play_ret == 0) ? pick_next_index_locked(RT_TRUE) : -1;
        if (next_idx >= 0)
        {
            rt_mutex_release(&g_mp.lock);
            cmd.type = MP_CMD_PLAY_IDX;
            cmd.arg  = next_idx;
            goto process_cmd;
        }
        g_mp.state = MUSIC_STATE_STOPPED;
        g_mp.elapsed_ms = 0U;
        g_mp.progress_permille = 0U;
        g_mp.has_resume_pos = RT_FALSE;
        g_mp.rev++;
        rt_mutex_release(&g_mp.lock);
        if (g_mp.waiting_preempt)
        {
            g_mp.waiting_preempt = RT_FALSE;
            rt_completion_done(&g_mp.preempt_ack);
        }
    }
}

/* ---------------- Public APIs ---------------- */

static void send_cmd(uint8_t type, int32_t arg)
{
    mp_cmd_t cmd = { .type = type, .arg = arg };
    if (!g_mp.inited || g_mp.cmd_mq == RT_NULL) return;

    g_mp.pending_cmd = type;
    g_mp.pending_arg = arg;
    g_mp.abort_cur = RT_TRUE;
    (void)rt_mq_send(g_mp.cmd_mq, &cmd, sizeof(cmd));
}

int music_player_init(void)
{
    if (g_mp.inited) return RT_EOK;

    rt_memset(&g_mp, 0, sizeof(g_mp));
    g_mp.current_index = -1;
    g_mp.mode = MUSIC_MODE_LOOP_ALL;
    g_mp.state = MUSIC_STATE_STOPPED;

    rt_mutex_init(&g_mp.lock, "mus_lk", RT_IPC_FLAG_PRIO);
    rt_mutex_init(&g_mp.preempt_lock, "mus_pre", RT_IPC_FLAG_PRIO);
    g_mp.cmd_mq = rt_mq_create("mus_mq", sizeof(mp_cmd_t), 16, RT_IPC_FLAG_FIFO);
    if (g_mp.cmd_mq == RT_NULL)
    {
        return -RT_ENOMEM;
    }

    g_mp.worker = rt_thread_create("music_wk", music_worker_entry, RT_NULL,
                                   MP_THREAD_STACK_SIZE, MP_THREAD_PRIO, 10);
    if (g_mp.worker == RT_NULL)
    {
        rt_mq_delete(g_mp.cmd_mq);
        g_mp.cmd_mq = RT_NULL;
        return -RT_ENOMEM;
    }

    g_mp.inited = RT_TRUE;
    rt_thread_startup(g_mp.worker);
    LOG_I("music player ready (WAV + MP3)");
    return RT_EOK;
}

int music_player_scan(void)
{
    if (!g_mp.inited) return -RT_ERROR;

    rt_mutex_take(&g_mp.lock, RT_WAITING_FOREVER);
    char previous_path[MUSIC_PATH_MAX] = "";
    uint32_t mount_gen = sdcard_get_mount_gen();
    if (mount_gen == g_mp.last_sd_gen && g_mp.current_index >= 0 &&
        g_mp.current_index < (int)g_mp.track_count)
        memcpy(previous_path, g_mp.tracks[g_mp.current_index].path, sizeof(previous_path));

    g_mp.track_count = 0U;
    g_mp.library_rev++;
    if (!sdcard_is_mounted())
    {
        g_mp.current_index = -1;
        g_mp.has_resume_pos = RT_FALSE;
        g_mp.rev++;
        rt_mutex_release(&g_mp.lock);
        return -RT_ENOENT;
    }

    scan_dir_locked(SD_MUSIC_DIR);
    scan_dir_locked("/sdcard");
    if (g_mp.track_count > 1U)
    {
        qsort(g_mp.tracks, g_mp.track_count, sizeof(g_mp.tracks[0]), track_cmp);
    }
    int previous_index = -1;
    for (uint32_t i = 0; previous_path[0] && i < g_mp.track_count; i++)
    {
        if (strcmp(previous_path, g_mp.tracks[i].path) == 0)
        {
            previous_index = (int)i;
            break;
        }
    }
    g_mp.current_index = previous_index >= 0 ? previous_index : (g_mp.track_count ? 0 : -1);
    if (previous_index < 0) g_mp.has_resume_pos = RT_FALSE;
    rebuild_shuffle_locked(previous_index);
    g_mp.last_sd_gen = mount_gen;
    g_mp.rev++;
    uint32_t cnt = g_mp.track_count;
    rt_mutex_release(&g_mp.lock);

    LOG_I("scanned %lu track(s) on TF card", (unsigned long)cnt);
    return (int)cnt;
}

int music_player_play_index(int index)
{
    if (!g_mp.inited) return -RT_ERROR;
    if (g_mp.track_count == 0U)
    {
        (void)music_player_scan();
    }
    if (g_mp.track_count == 0U) return -RT_ENOENT;
    if (index < 0 || index >= (int)g_mp.track_count) index = 0;
    g_mp.seek_ms_pending = RT_FALSE;
    send_cmd(MP_CMD_PLAY_IDX, index);
    return RT_EOK;
}

int music_player_toggle_pause(void)
{
    if (!g_mp.inited) return -RT_ERROR;
    if (g_mp.state == MUSIC_STATE_PLAYING)
    {
        return music_player_pause_playback();
    }
    return music_player_resume();
}

int music_player_pause_playback(void)
{
    if (!g_mp.inited) return -RT_ERROR;
    if (g_mp.state == MUSIC_STATE_PLAYING)
    {
        send_cmd(MP_CMD_PAUSE, 0);
    }
    return RT_EOK;
}

int music_player_resume(void)
{
    if (!g_mp.inited) return -RT_ERROR;
    if (g_mp.track_count == 0U)
    {
        (void)music_player_scan();
    }
    if (g_mp.track_count == 0U) return -RT_ENOENT;
    if (g_mp.state == MUSIC_STATE_PAUSED)
    {
        send_cmd(MP_CMD_RESUME, 0);
    }
    else if (g_mp.state == MUSIC_STATE_STOPPED)
    {
        int idx = (g_mp.current_index >= 0) ? g_mp.current_index : 0;
        send_cmd(MP_CMD_PLAY_IDX, idx);
    }
    return RT_EOK;
}

int music_player_stop(void)
{
    if (!g_mp.inited) return -RT_ERROR;
    g_mp.seek_ms_pending = RT_FALSE;
    send_cmd(MP_CMD_STOP, 0);
    return RT_EOK;
}

int music_player_next(void)
{
    if (!g_mp.inited) return -RT_ERROR;
    if (g_mp.track_count == 0U) (void)music_player_scan();
    if (g_mp.track_count == 0U) return -RT_ENOENT;
    g_mp.seek_ms_pending = RT_FALSE;
    send_cmd(MP_CMD_NEXT, 0);
    return RT_EOK;
}

int music_player_prev(void)
{
    if (!g_mp.inited) return -RT_ERROR;
    if (g_mp.track_count == 0U) (void)music_player_scan();
    if (g_mp.track_count == 0U) return -RT_ENOENT;
    g_mp.seek_ms_pending = RT_FALSE;
    send_cmd(MP_CMD_PREV, 0);
    return RT_EOK;
}

int music_player_seek_permille(uint16_t permille)
{
    if (!g_mp.inited || g_mp.state != MUSIC_STATE_PLAYING) return -RT_ERROR;
    if (permille > 1000U) permille = 1000U;
    g_mp.seek_ms_pending = RT_FALSE;
    g_mp.seek_permille_req = permille;
    g_mp.seek_pending = RT_TRUE;
    return RT_EOK;
}

int music_player_seek_ms(uint32_t target_ms)
{
    if (!g_mp.inited) return -RT_ERROR;
    if (g_mp.track_count == 0U) (void)music_player_scan();
    if (g_mp.track_count == 0U) return -RT_ENOENT;
    g_mp.seek_pending = RT_FALSE;
    g_mp.seek_ms_req = target_ms;
    g_mp.seek_ms_pending = RT_TRUE;
    if (g_mp.state != MUSIC_STATE_PLAYING)
    {
        int idx = (g_mp.current_index >= 0 && g_mp.current_index < (int)g_mp.track_count)
                      ? g_mp.current_index
                      : 0;
        send_cmd(MP_CMD_PLAY_IDX, idx);
    }
    return RT_EOK;
}

void music_player_set_mode(music_play_mode_t mode)
{
    if (mode >= MUSIC_MODE_COUNT) return;
    rt_mutex_take(&g_mp.lock, RT_WAITING_FOREVER);
    g_mp.mode = mode;
    if (mode == MUSIC_MODE_SHUFFLE)
    {
        rebuild_shuffle_locked(g_mp.current_index);
    }
    g_mp.rev++;
    rt_mutex_release(&g_mp.lock);
}

music_play_mode_t music_player_get_mode(void)
{
    return g_mp.mode;
}

const char *music_player_mode_name(music_play_mode_t mode)
{
    switch (mode)
    {
    case MUSIC_MODE_SEQ:      return "顺序播放";
    case MUSIC_MODE_LOOP_ALL: return "列表循环";
    case MUSIC_MODE_LOOP_ONE: return "单曲循环";
    case MUSIC_MODE_SHUFFLE:  return "随机播放";
    default:                  return "列表循环";
    }
}

void music_player_preempt(void)
{
    if (!g_mp.inited) return;
    rt_mutex_take(&g_mp.preempt_lock, RT_WAITING_FOREVER);
    if (g_mp.state == MUSIC_STATE_PLAYING || g_mp.sound_opened || g_mp.worker_active)
    {
        rt_completion_init(&g_mp.preempt_ack);
        g_mp.waiting_preempt = RT_TRUE;
        send_cmd(MP_CMD_PREEMPT, 0);
        (void)rt_completion_wait(&g_mp.preempt_ack, rt_tick_from_millisecond(1000));
        g_mp.waiting_preempt = RT_FALSE;
    }
    rt_mutex_release(&g_mp.preempt_lock);
}

uint32_t music_player_get_track_count(void)
{
    return g_mp.track_count;
}

rt_bool_t music_player_get_track(uint32_t index, music_track_info_t *out)
{
    if (out == RT_NULL || !g_mp.inited) return RT_FALSE;
    rt_mutex_take(&g_mp.lock, RT_WAITING_FOREVER);
    if (index >= g_mp.track_count)
    {
        rt_mutex_release(&g_mp.lock);
        return RT_FALSE;
    }
    *out = g_mp.tracks[index];
    rt_mutex_release(&g_mp.lock);
    return RT_TRUE;
}

void music_player_get_status(music_status_t *out)
{
    if (out == RT_NULL) return;
    if (!g_mp.inited)
    {
        rt_memset(out, 0, sizeof(*out));
        out->current_index = -1;
        return;
    }

    rt_mutex_take(&g_mp.lock, RT_WAITING_FOREVER);
    out->state             = g_mp.state;
    out->mode              = g_mp.mode;
    out->track_count       = g_mp.track_count;
    out->current_index     = g_mp.current_index;
    out->sample_rate       = g_mp.sample_rate;
    out->channels          = g_mp.channels;
    out->elapsed_sec       = g_mp.elapsed_ms / 1000U;
    out->elapsed_ms        = g_mp.elapsed_ms;
    out->duration_sec      = g_mp.duration_sec;
    out->progress_permille = g_mp.progress_permille;
    out->sd_mounted        = sdcard_is_mounted();
    out->library_rev       = g_mp.library_rev;
    out->rev               = g_mp.rev;
    if (g_mp.current_index >= 0 && g_mp.current_index < (int)g_mp.track_count)
    {
        rt_strncpy(out->title, g_mp.tracks[g_mp.current_index].title, sizeof(out->title) - 1U);
        out->title[sizeof(out->title) - 1U] = '\0';
        out->format = g_mp.tracks[g_mp.current_index].format;
    }
    else
    {
        rt_strncpy(out->title, "未选择歌曲", sizeof(out->title) - 1U);
        out->title[sizeof(out->title) - 1U] = '\0';
        out->format = MUSIC_FMT_NONE;
    }
    rt_mutex_release(&g_mp.lock);
}

/* ---------------- MSH Commands ---------------- */

static int cmd_music_scan(void)
{
    int cnt = music_player_scan();
    rt_kprintf("music_scan: %d track(s) (sd=%d)\n", cnt, sdcard_is_mounted() ? 1 : 0);
    return RT_EOK;
}
MSH_CMD_EXPORT_ALIAS(cmd_music_scan, music_scan, Scan TF card music tracks);

static int cmd_music_list(void)
{
    music_status_t st;
    music_player_get_status(&st);
    rt_kprintf("Tracks (%lu, sd=%d):\n", (unsigned long)st.track_count, st.sd_mounted ? 1 : 0);
    for (uint32_t i = 0U; i < st.track_count; i++)
    {
        music_track_info_t tr;
        if (music_player_get_track(i, &tr))
        {
            rt_kprintf("  [%lu] %s (%s, %lu B)\n",
                       (unsigned long)i, tr.title,
                       tr.format == MUSIC_FMT_MP3 ? "MP3" : "WAV",
                       (unsigned long)tr.file_size);
        }
    }
    return RT_EOK;
}
MSH_CMD_EXPORT_ALIAS(cmd_music_list, music_list, List scanned TF card songs);

static int cmd_music_play(int argc, char **argv)
{
    int idx = (argc >= 2) ? atoi(argv[1]) : 0;
    return music_player_play_index(idx);
}
MSH_CMD_EXPORT_ALIAS(cmd_music_play, music_play, Play TF card track by index);

static int cmd_music_pause(void) { return music_player_toggle_pause(); }
MSH_CMD_EXPORT_ALIAS(cmd_music_pause, music_pause, Toggle pause/resume music);

static int cmd_music_stop(void) { return music_player_stop(); }
MSH_CMD_EXPORT_ALIAS(cmd_music_stop, music_stop, Stop music playback);

static int cmd_music_next(void) { return music_player_next(); }
MSH_CMD_EXPORT_ALIAS(cmd_music_next, music_next, Next track);

static int cmd_music_prev(void) { return music_player_prev(); }
MSH_CMD_EXPORT_ALIAS(cmd_music_prev, music_prev, Previous track);

static int cmd_music_mode(int argc, char **argv)
{
    if (argc >= 2)
    {
        if (strcmp(argv[1], "seq") == 0)           music_player_set_mode(MUSIC_MODE_SEQ);
        else if (strcmp(argv[1], "loop_all") == 0) music_player_set_mode(MUSIC_MODE_LOOP_ALL);
        else if (strcmp(argv[1], "loop_one") == 0) music_player_set_mode(MUSIC_MODE_LOOP_ONE);
        else if (strcmp(argv[1], "shuffle") == 0)  music_player_set_mode(MUSIC_MODE_SHUFFLE);
        else
        {
            rt_kprintf("Usage: music_mode <seq|loop_all|loop_one|shuffle>\n");
            return -RT_EINVAL;
        }
    }
    rt_kprintf("music_mode=%s\n", music_player_mode_name(music_player_get_mode()));
    return RT_EOK;
}
MSH_CMD_EXPORT_ALIAS(cmd_music_mode, music_mode, Set or show music play mode);

static int cmd_music_status(void)
{
    music_status_t st;
    music_player_get_status(&st);
    rt_kprintf("music state=%d mode=%s tracks=%lu cur=%d title=%s sr=%lu ch=%u %lu/%lus\n",
               (int)st.state, music_player_mode_name(st.mode),
               (unsigned long)st.track_count, st.current_index, st.title,
               (unsigned long)st.sample_rate, (unsigned)st.channels,
               (unsigned long)st.elapsed_sec, (unsigned long)st.duration_sec);
    return RT_EOK;
}
MSH_CMD_EXPORT_ALIAS(cmd_music_status, music_status, Show music player status);

static int cmd_music_seek(int argc, char **argv)
{
    if (argc < 2)
    {
        rt_kprintf("Usage: music_seek <seconds>\n");
        return -RT_EINVAL;
    }
    long sec = atol(argv[1]);
    if (sec < 0) sec = 0;
    return music_player_seek_ms((uint32_t)sec * 1000U);
}
MSH_CMD_EXPORT_ALIAS(cmd_music_seek, music_seek, Seek current track to seconds);
