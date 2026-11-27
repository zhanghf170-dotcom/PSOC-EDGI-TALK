#include <lvgl.h>
#include <rtthread.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "src/libs/lodepng/lodepng.h"
#include "music_player.h"
#include "sd_assets.h"
#include "tjpgd.h"
#include "ui_music_cover.h"

#define DBG_TAG "music.cover"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

#define COVER_FILE_LIMIT (4U * 1024U * 1024U)
#define COVER_JPEG_EDGE_LIMIT 2048U
#define COVER_PNG_EDGE_LIMIT 512U
#define COVER_PNG_FILE_LIMIT (512U * 1024U)

typedef struct {
    long start;
    uint32_t length;
} cover_range_t;

typedef struct {
    FILE *file;
    uint32_t remaining;
    unsigned crop_x;
    unsigned crop_y;
    unsigned crop_size;
} cover_jpeg_t;

static struct rt_mutex g_cover_lock;
static struct rt_semaphore g_cover_wake;
static bool g_cover_inited;
static char g_cover_path[MUSIC_PATH_MAX];
static uint32_t g_cover_request;
static uint32_t g_cover_mount;
static music_cover_state_t g_cover_state;
/* One worker owns the decode buffers. LVGL gets its own copy on completion. */
static uint32_t g_cover_pixels[MUSIC_COVER_SIZE * MUSIC_COVER_SIZE]
    __attribute__((section(".cy_socmem_data"), aligned(64)));
static uint8_t g_jpeg_work[4096] __attribute__((section(".cy_socmem_data"), aligned(8)));
static music_lyrics_t g_loaded_lyrics __attribute__((section(".cy_socmem_data")));

static uint32_t read_be32(const uint8_t *b)
{
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
}

static uint32_t read_syncsafe(const uint8_t *b)
{
    if ((b[0] | b[1] | b[2] | b[3]) & 0x80U) return UINT32_MAX;
    return ((uint32_t)b[0] << 21) | ((uint32_t)b[1] << 14) | ((uint32_t)b[2] << 7) | b[3];
}

static bool skip_id3_string(FILE *file, long end, bool utf16)
{
    long pos = ftell(file);
    unsigned step = utf16 ? 2U : 1U;
    if (pos < 0) return false;
    /* A picture description should be short. Keep reads within the frame. */
    for (unsigned i = 0; i < 1024U && end - pos >= (long)step; i += step, pos += step)
    {
        int a = fgetc(file);
        int b = utf16 ? fgetc(file) : 0;
        if (a == EOF || b == EOF) return false;
        if (!a && !b) return true;
    }
    return false;
}

static bool cover_magic(const uint8_t *signature)
{
    return (signature[0] == 0xff && signature[1] == 0xd8) ||
           memcmp(signature, "\x89PNG\r\n\x1a\n", 8) == 0;
}

static bool id3_picture(FILE *file, long end, cover_range_t *range, int *type)
{
    int encoding = fgetc(file);
    if (encoding < 0 || encoding > 3 || !skip_id3_string(file, end, false)) return false;
    if (ftell(file) >= end) return false;
    *type = fgetc(file);
    if (*type == EOF || !skip_id3_string(file, end, encoding == 1 || encoding == 2)) return false;
    long start = ftell(file);
    if (start < 0 || end - start < 8 || end - start > (long)COVER_FILE_LIMIT) return false;
    uint8_t signature[8];
    if (fread(signature, 1U, sizeof(signature), file) != sizeof(signature) || !cover_magic(signature))
        return false;
    *range = (cover_range_t){ .start = start, .length = (uint32_t)(end - start) };
    return true;
}

static cover_range_t find_embedded_cover(FILE *file, long file_size)
{
    uint8_t header[10];
    cover_range_t other = {0};
    if (fread(header, 1U, sizeof(header), file) != sizeof(header) ||
        memcmp(header, "ID3", 3) != 0 || (header[3] != 3 && header[3] != 4) ||
        (header[5] & 0x80U)) return other;

    bool v24 = header[3] == 4;
    uint32_t tag_size = read_syncsafe(header + 6);
    if (file_size < 10 || tag_size > (uint32_t)(file_size - 10) ||
        tag_size > 2U * COVER_FILE_LIMIT) return other;
    long end = 10 + (long)tag_size;
    long pos = 10;
    if (header[5] & 0x40U)
    {
        uint8_t size[4];
        if (tag_size < 4 || fread(size, 1U, sizeof(size), file) != sizeof(size)) return other;
        uint32_t extended = v24 ? read_syncsafe(size) : read_be32(size);
        if (extended > tag_size || (v24 && extended < 4U)) return other;
        pos += (long)extended + (v24 ? 0L : 4L);
    }

    for (unsigned frames = 0; frames < 256U && end - pos >= 10; frames++)
    {
        if (fseek(file, pos, SEEK_SET) != 0 ||
            fread(header, 1U, sizeof(header), file) != sizeof(header) || !header[0])
            break;
        uint32_t size = v24 ? read_syncsafe(header + 4) : read_be32(header + 4);
        if (!size || size > (uint32_t)(end - pos - 10)) break;
        pos += 10 + (long)size;
        /* Encoded, compressed, unsynchronized and grouped frames need separate handling. */
        if (memcmp(header, "APIC", 4) != 0 || (header[9] & (v24 ? 0x4fU : 0xe0U))) continue;
        int type;
        cover_range_t range;
        if (id3_picture(file, pos, &range, &type))
        {
            if (type == 3) return range;
            if (!other.length) other = range;
        }
    }
    return other;
}

/* JPEG blocks are cropped and downsampled directly into the circular label. */
static size_t jpeg_input(JDEC *jd, uint8_t *buf, size_t len)
{
    cover_jpeg_t *ctx = jd->device;
    if (len > ctx->remaining) len = ctx->remaining;
    if (buf) len = fread(buf, 1, len, ctx->file);
    else if (fseek(ctx->file, (long)len, SEEK_CUR) != 0) return 0;
    ctx->remaining -= len;
    return len;
}

static bool inside_label(unsigned x, unsigned y)
{
    int dx = 2 * (int)x + 1 - (int)MUSIC_COVER_SIZE;
    int dy = 2 * (int)y + 1 - (int)MUSIC_COVER_SIZE;
    return dx * dx + dy * dy <= (int)(MUSIC_COVER_SIZE * MUSIC_COVER_SIZE);
}

static unsigned crop_sample(unsigned origin, unsigned size, unsigned output)
{
    return origin + (2U * output + 1U) * size / (2U * MUSIC_COVER_SIZE);
}

static unsigned crop_edge(unsigned origin, unsigned size, unsigned edge)
{
    if (edge <= origin) return 0;
    if (edge >= origin + size) return MUSIC_COVER_SIZE;
    /* First destination sample at or past the given source coordinate. */
    unsigned scaled = (edge - origin) * MUSIC_COVER_SIZE * 2U;
    return (scaled + size - 1U) / (2U * size);
}

static int jpeg_output(JDEC *jd, void *block, JRECT *rect)
{
    const cover_jpeg_t *ctx = jd->device;
    const uint16_t *src = block; /* Board TJpgDec is configured for RGB565. */
    unsigned width = rect->right + 1U - rect->left;
    unsigned x_start = crop_edge(ctx->crop_x, ctx->crop_size, rect->left);
    unsigned x_end = crop_edge(ctx->crop_x, ctx->crop_size, rect->right + 1U);
    unsigned y_start = crop_edge(ctx->crop_y, ctx->crop_size, rect->top);
    unsigned y_end = crop_edge(ctx->crop_y, ctx->crop_size, rect->bottom + 1U);
    for (unsigned y = y_start; y < y_end; y++)
    {
        unsigned sy = crop_sample(ctx->crop_y, ctx->crop_size, y) - rect->top;
        for (unsigned x = x_start; x < x_end; x++)
        {
            if (!inside_label(x, y)) continue;
            unsigned sx = crop_sample(ctx->crop_x, ctx->crop_size, x) - rect->left;
            uint16_t pixel = src[sy * width + sx];
            uint32_t r = (pixel >> 11) & 31U;
            uint32_t g = (pixel >> 5) & 63U;
            uint32_t b = pixel & 31U;
            g_cover_pixels[y * MUSIC_COVER_SIZE + x] =
                0xff000000U | ((r << 3 | r >> 2) << 16) | ((g << 2 | g >> 4) << 8) | (b << 3 | b >> 2);
        }
    }
    return 1;
}

static bool decode_jpeg(FILE *file, cover_range_t range)
{
    if (fseek(file, range.start, SEEK_SET) != 0) return false;
    cover_jpeg_t ctx = { .file = file, .remaining = range.length };
    JDEC decoder;
    if (jd_prepare(&decoder, jpeg_input, g_jpeg_work, sizeof(g_jpeg_work), &ctx) != JDR_OK ||
        decoder.width > COVER_JPEG_EDGE_LIMIT || decoder.height > COVER_JPEG_EDGE_LIMIT)
        return false;

    ctx.crop_size = decoder.width < decoder.height ? decoder.width : decoder.height;
    ctx.crop_x = (decoder.width - ctx.crop_size) / 2U;
    ctx.crop_y = (decoder.height - ctx.crop_size) / 2U;
    memset(g_cover_pixels, 0, sizeof(g_cover_pixels));
    return jd_decomp(&decoder, jpeg_output, 0) == JDR_OK;
}

static bool decode_png(FILE *file, cover_range_t range)
{
    uint8_t header[29];
    if (range.length < sizeof(header) || range.length > COVER_PNG_FILE_LIMIT ||
        fseek(file, range.start, SEEK_SET) != 0 ||
        fread(header, 1U, sizeof(header), file) != sizeof(header))
        return false;
    unsigned width = read_be32(header + 16);
    unsigned height = read_be32(header + 20);
    if (!width || !height || width > COVER_PNG_EDGE_LIMIT || height > COVER_PNG_EDGE_LIMIT ||
        header[24] > 8U || fseek(file, range.start, SEEK_SET) != 0) return false;

    uint8_t *encoded = lv_malloc(range.length);
    if (!encoded) return false;
    lv_draw_buf_t *decoded = NULL;
    if (fread(encoded, 1U, range.length, file) == range.length)
    {
        LodePNGState state;
        lodepng_state_init(&state);
        state.decoder.read_text_chunks = 0;
        state.decoder.zlibsettings.max_output_size = width * height * 4U + height * 8U;
        unsigned char *output = NULL;
        unsigned error = lodepng_decode(&output, &width, &height, &state, encoded, range.length);
        lodepng_state_cleanup(&state);
        decoded = (lv_draw_buf_t *)output;
        if (error && decoded)
        {
            lv_draw_buf_destroy(decoded);
            decoded = NULL;
        }
    }
    lv_free(encoded);
    if (!decoded) return false;

    unsigned size = width < height ? width : height;
    memset(g_cover_pixels, 0, sizeof(g_cover_pixels));
    for (unsigned y = 0; y < MUSIC_COVER_SIZE; y++)
    {
        unsigned sy = crop_sample((height - size) / 2U, size, y);
        const uint8_t *row = decoded->data + sy * decoded->header.stride;
        for (unsigned x = 0; x < MUSIC_COVER_SIZE; x++)
        {
            if (!inside_label(x, y)) continue;
            unsigned sx = crop_sample((width - size) / 2U, size, x);
            const uint8_t *p = row + sx * 4U;
            g_cover_pixels[y * MUSIC_COVER_SIZE + x] =
                ((uint32_t)p[3] << 24) | ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
        }
    }
    lv_draw_buf_destroy(decoded);
    return true;
}

static bool decode_cover(FILE *file, cover_range_t range)
{
    uint8_t signature[8];
    if (range.length < sizeof(signature) || fseek(file, range.start, SEEK_SET) != 0 ||
        fread(signature, 1U, sizeof(signature), file) != sizeof(signature) || !cover_magic(signature))
        return false;
    return signature[0] == 0xff ? decode_jpeg(file, range) : decode_png(file, range);
}

static const char *load_cover(const char *path)
{
    char sidecar[MUSIC_PATH_MAX + 1U];
    const char *extension = strrchr(path, '.');
    if (!extension) return NULL;

    static const char *const formats[] = {".jpg", ".jpeg", ".png"};
    for (unsigned i = 0; i < sizeof(formats) / sizeof(formats[0]); i++)
    {
        snprintf(sidecar, sizeof(sidecar), "%.*s%s", (int)(extension - path), path, formats[i]);
        FILE *file = fopen(sidecar, "rb");
        if (!file) continue;
        long size = fseek(file, 0, SEEK_END) == 0 ? ftell(file) : -1L;
        bool ok = size > 0 && size <= (long)COVER_FILE_LIMIT &&
                  decode_cover(file, (cover_range_t){ .length = (uint32_t)size });
        fclose(file);
        if (ok) return "SD sidecar";
    }

    if (strcasecmp(extension, ".mp3") != 0) return NULL;
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    long size = fseek(file, 0, SEEK_END) == 0 ? ftell(file) : -1L;
    cover_range_t range = {0};
    if (size > 0 && fseek(file, 0, SEEK_SET) == 0) range = find_embedded_cover(file, size);
    bool ok = decode_cover(file, range);
    fclose(file);
    return ok ? "embedded ID3" : NULL;
}

static void cover_worker(void *arg)
{
    (void)arg;
    uint32_t completed = 0;
    while (true)
    {
        rt_sem_take(&g_cover_wake, RT_WAITING_FOREVER);
        rt_mutex_take(&g_cover_lock, RT_WAITING_FOREVER);
        uint32_t request = g_cover_request;
        uint32_t mount = g_cover_mount;
        char path[MUSIC_PATH_MAX];
        memcpy(path, g_cover_path, sizeof(path));
        rt_mutex_release(&g_cover_lock);
        if (request == completed) continue;
        completed = request;

        const char *source = NULL;
        g_loaded_lyrics.count = 0U;
        if (path[0] && sdcard_is_mounted() && mount == sdcard_get_mount_gen())
        {
            ui_music_lyrics_load(path, &g_loaded_lyrics);
            source = load_cover(path);
        }

        rt_mutex_take(&g_cover_lock, RT_WAITING_FOREVER);
        bool current = request == g_cover_request;
        bool card_current = sdcard_is_mounted() && mount == sdcard_get_mount_gen();
        bool ready = source && card_current;
        unsigned lines = card_current ? g_loaded_lyrics.count : 0U;
        if (!card_current) g_loaded_lyrics.count = 0;
        if (current) g_cover_state = ready ? MUSIC_COVER_READY : MUSIC_COVER_NONE;
        rt_mutex_release(&g_cover_lock);
        if (current && path[0])
        {
            if (ready) LOG_I("album art ready (%s): %s", source, path);
            else LOG_I("album art unavailable: %s", path);
            if (lines) LOG_I("loaded %u synced lyric lines", lines);
        }
    }
}

void ui_music_cover_request(const char *path, uint32_t mount_gen)
{
    if (!g_cover_inited)
    {
        if (!path || !path[0]) return;
        rt_mutex_init(&g_cover_lock, "cover", RT_IPC_FLAG_PRIO);
        rt_sem_init(&g_cover_wake, "cover", 0, RT_IPC_FLAG_PRIO);
        rt_thread_t thread = rt_thread_create("album_art", cover_worker, NULL, 6144U, 22, 10);
        if (!thread)
        {
            rt_sem_detach(&g_cover_wake);
            rt_mutex_detach(&g_cover_lock);
            return;
        }
        g_cover_inited = true;
        rt_thread_startup(thread);
    }

    rt_mutex_take(&g_cover_lock, RT_WAITING_FOREVER);
    snprintf(g_cover_path, sizeof(g_cover_path), "%s", path ? path : "");
    g_cover_mount = mount_gen;
    g_cover_request++;
    g_cover_state = g_cover_path[0] ? MUSIC_COVER_LOADING : MUSIC_COVER_NONE;
    rt_mutex_release(&g_cover_lock);
    rt_sem_release(&g_cover_wake);
}

music_cover_state_t ui_music_cover_copy(uint32_t *argb, music_lyrics_t *lyrics)
{
    if (!g_cover_inited) return MUSIC_COVER_NONE;
    rt_mutex_take(&g_cover_lock, RT_WAITING_FOREVER);
    music_cover_state_t state = g_cover_state;
    if (state == MUSIC_COVER_READY) memcpy(argb, g_cover_pixels, sizeof(g_cover_pixels));
    if (state != MUSIC_COVER_LOADING)
    {
        lyrics->count = g_loaded_lyrics.count;
        memcpy(lyrics->lines, g_loaded_lyrics.lines, lyrics->count * sizeof(lyrics->lines[0]));
    }
    rt_mutex_release(&g_cover_lock);
    return state;
}
