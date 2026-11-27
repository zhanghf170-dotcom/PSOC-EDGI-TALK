/*
 * Fast, reentrant baseline JFIF JPEG encoder (4:2:2 subsampling, integer/FPU AAN DCT)
 * Compliant with ITU-T T.81 / JFIF for HTTP MJPEG & TCP snapshot streaming.
 */

#include "mini_jpeg_enc.h"
#include <string.h>

#if defined(__RTTHREAD__) && !defined(MINI_JPEG_HOST_TEST)
#include <rtthread.h>
#endif

#if defined(__ARM_ARCH) && !defined(MINI_JPEG_HOST_TEST)
#define JPEG_ITCM_SECTION   __attribute__((section(".cy_itcm")))
#define JPEG_SOCMEM_SECTION __attribute__((section(".cy_socmem_data")))
#else
#define JPEG_ITCM_SECTION
#define JPEG_SOCMEM_SECTION
#endif

/* Natural-order Annex K.1 standard luminance quantization table */
static const uint8_t g_std_luma_qt[64] = {
    16, 11, 10, 16,  24,  40,  51,  61,
    12, 12, 14, 19,  26,  58,  60,  55,
    14, 13, 16, 24,  40,  57,  69,  56,
    14, 17, 22, 29,  51,  87,  80,  62,
    18, 22, 37, 56,  68, 109, 103,  77,
    24, 35, 55, 64,  81, 104, 113,  92,
    49, 64, 78, 87, 103, 121, 120, 101,
    72, 92, 95, 98, 112, 100, 103,  99
};

/* Natural-order Annex K.2 standard chrominance quantization table */
static const uint8_t g_std_chroma_qt[64] = {
    17, 18, 24, 47, 99, 99, 99, 99,
    18, 21, 26, 66, 99, 99, 99, 99,
    24, 26, 56, 99, 99, 99, 99, 99,
    47, 66, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99
};

/* Zigzag index mapping: zz[i] -> row-major index */
static const uint8_t g_zigzag[64] = {
     0,  1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63
};

/* Standard Annex K.3/K.4/K.5 Huffman specs */
static const uint8_t g_dc_luma_nrcodes[17] = {
    0, 0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0
};
static const uint8_t g_dc_luma_vals[12] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11
};

static const uint8_t g_dc_chroma_nrcodes[17] = {
    0, 0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0
};
static const uint8_t g_dc_chroma_vals[12] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11
};

static const uint8_t g_ac_luma_nrcodes[17] = {
    0, 0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d
};
static const uint8_t g_ac_luma_vals[162] = {
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12,
    0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07,
    0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08,
    0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0,
    0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16,
    0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28,
    0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39,
    0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
    0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59,
    0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
    0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79,
    0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
    0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98,
    0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
    0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6,
    0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5,
    0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4,
    0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
    0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea,
    0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
    0xf9, 0xfa
};

static const uint8_t g_ac_chroma_nrcodes[17] = {
    0, 0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77
};
static const uint8_t g_ac_chroma_vals[162] = {
    0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21,
    0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71,
    0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91,
    0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0,
    0x15, 0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34,
    0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26,
    0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38,
    0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
    0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58,
    0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68,
    0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78,
    0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
    0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96,
    0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5,
    0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4,
    0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3,
    0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2,
    0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda,
    0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9,
    0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
    0xf9, 0xfa
};

static const float g_aan_scales[8] = {
    1.0f, 1.387039845f, 1.306562965f, 1.175875602f,
    1.0f, 0.785694958f, 0.541196100f, 0.275899379f
};

typedef struct
{
    uint16_t code;
    uint8_t  len;
} huff_sym_t;

typedef struct
{
    huff_sym_t dc_luma[12];
    huff_sym_t dc_chroma[12];
    huff_sym_t ac_luma[256];
    huff_sym_t ac_chroma[256];
    uint8_t    qt_luma_zz[64];
    uint8_t    qt_chroma_zz[64];
    float      inv_luma[64];
    float      inv_chroma[64];
} jpeg_tables_t;

typedef struct
{
    uint8_t *buf;
    size_t   cap;
    size_t   pos;
    uint32_t bitbuf;
    int      bitcnt;
    int      overflow;
} jpeg_bitwriter_t;

static void jpeg_build_huff_table(const uint8_t nrcodes[17], const uint8_t *vals, huff_sym_t *table)
{
    int p = 0;
    uint16_t code = 0;
    int bits, j;

    for (bits = 1; bits <= 16; bits++)
    {
        for (j = 0; j < nrcodes[bits]; j++)
        {
            uint8_t sym = vals[p++];
            table[sym].code = code;
            table[sym].len  = (uint8_t)bits;
            code++;
        }
        code <<= 1;
    }
}

static void jpeg_init_tables(jpeg_tables_t *t, int quality)
{
    int scale;
    int i, r, c;

    memset(t, 0, sizeof(*t));
    if (quality < 1) quality = 1;
    if (quality > 100) quality = 100;

    if (quality < 50)
    {
        scale = 5000 / quality;
    }
    else
    {
        scale = 200 - quality * 2;
    }

    for (i = 0; i < 64; i++)
    {
        int qy = (g_std_luma_qt[g_zigzag[i]] * scale + 50) / 100;
        int qc = (g_std_chroma_qt[g_zigzag[i]] * scale + 50) / 100;
        if (qy < 1) qy = 1;
        if (qy > 255) qy = 255;
        if (qc < 1) qc = 1;
        if (qc > 255) qc = 255;
        t->qt_luma_zz[i]   = (uint8_t)qy;
        t->qt_chroma_zz[i] = (uint8_t)qc;
    }

    /* Precompute row-major reciprocal factors incorporating AAN scale */
    for (r = 0; r < 8; r++)
    {
        for (c = 0; c < 8; c++)
        {
            int idx = r * 8 + c;
            int qy_nat = (g_std_luma_qt[idx] * scale + 50) / 100;
            int qc_nat = (g_std_chroma_qt[idx] * scale + 50) / 100;
            if (qy_nat < 1) qy_nat = 1;
            if (qy_nat > 255) qy_nat = 255;
            if (qc_nat < 1) qc_nat = 1;
            if (qc_nat > 255) qc_nat = 255;

            float aan = 8.0f * g_aan_scales[r] * g_aan_scales[c];
            t->inv_luma[idx]   = 1.0f / (aan * (float)qy_nat);
            t->inv_chroma[idx] = 1.0f / (aan * (float)qc_nat);
        }
    }

    jpeg_build_huff_table(g_dc_luma_nrcodes,   g_dc_luma_vals,   t->dc_luma);
    jpeg_build_huff_table(g_dc_chroma_nrcodes, g_dc_chroma_vals, t->dc_chroma);
    jpeg_build_huff_table(g_ac_luma_nrcodes,   g_ac_luma_vals,   t->ac_luma);
    jpeg_build_huff_table(g_ac_chroma_nrcodes, g_ac_chroma_vals, t->ac_chroma);
}

/* Cache quantization & Huffman tables in SoCMEM (saves 2,784 B off wf_enc stack and avoids DTCM). */
static JPEG_SOCMEM_SECTION jpeg_tables_t g_jpeg_tbl_cache;
static JPEG_SOCMEM_SECTION int g_jpeg_tbl_quality = -1;
#if defined(__RTTHREAD__) && !defined(MINI_JPEG_HOST_TEST)
static struct rt_mutex g_jpeg_tbl_mutex;
static rt_bool_t g_jpeg_tbl_lock_ready = RT_FALSE;

static int mini_jpeg_lock_init(void)
{
    if (rt_mutex_init(&g_jpeg_tbl_mutex, "jpg_tbl", RT_IPC_FLAG_PRIO) == RT_EOK)
    {
        g_jpeg_tbl_lock_ready = RT_TRUE;
    }
    return RT_EOK;
}
INIT_COMPONENT_EXPORT(mini_jpeg_lock_init);
#endif

static const jpeg_tables_t *jpeg_acquire_tables(int quality)
{
    if (quality < 1) quality = 1;
    if (quality > 100) quality = 100;

#if defined(__RTTHREAD__) && !defined(MINI_JPEG_HOST_TEST)
    if (g_jpeg_tbl_lock_ready && rt_thread_self() != RT_NULL && rt_interrupt_get_nest() == 0U)
    {
        (void)rt_mutex_take(&g_jpeg_tbl_mutex, RT_WAITING_FOREVER);
    }
#endif
    if (g_jpeg_tbl_quality != quality)
    {
        jpeg_init_tables(&g_jpeg_tbl_cache, quality);
        g_jpeg_tbl_quality = quality;
    }
    return &g_jpeg_tbl_cache;
}

static void jpeg_release_tables(void)
{
#if defined(__RTTHREAD__) && !defined(MINI_JPEG_HOST_TEST)
    if (g_jpeg_tbl_lock_ready && rt_thread_self() != RT_NULL &&
        rt_interrupt_get_nest() == 0U && g_jpeg_tbl_mutex.owner == rt_thread_self())
    {
        (void)rt_mutex_release(&g_jpeg_tbl_mutex);
    }
#endif
}

static inline void bw_put_byte(jpeg_bitwriter_t *bw, uint8_t b)
{
    if (bw->pos < bw->cap)
    {
        bw->buf[bw->pos++] = b;
    }
    else
    {
        bw->overflow = 1;
    }
}

static inline void bw_put_u16(jpeg_bitwriter_t *bw, uint16_t v)
{
    bw_put_byte(bw, (uint8_t)(v >> 8));
    bw_put_byte(bw, (uint8_t)(v & 0xffU));
}

static JPEG_ITCM_SECTION void bw_put_bits(jpeg_bitwriter_t *bw, uint16_t code, int nbits)
{
    bw->bitbuf = (bw->bitbuf << nbits) | (uint32_t)code;
    bw->bitcnt += nbits;

    while (bw->bitcnt >= 8)
    {
        uint8_t b = (uint8_t)((bw->bitbuf >> (bw->bitcnt - 8)) & 0xffU);
        bw->bitcnt -= 8;
        bw_put_byte(bw, b);
        if (b == 0xffU)
        {
            bw_put_byte(bw, 0x00U);
        }
    }
}

static void bw_flush_bits(jpeg_bitwriter_t *bw)
{
    if (bw->bitcnt > 0)
    {
        int pad = 8 - bw->bitcnt;
        uint16_t fill = (uint16_t)((1U << pad) - 1U);
        bw_put_bits(bw, fill, pad);
    }
}

static void jpeg_write_dht(jpeg_bitwriter_t *bw, uint8_t id,
                           const uint8_t nrcodes[17], const uint8_t *vals, int nvals)
{
    bw_put_u16(bw, 0xFFC4U);
    bw_put_u16(bw, (uint16_t)(2 + 1 + 16 + nvals));
    bw_put_byte(bw, id);
    for (int i = 1; i <= 16; i++) bw_put_byte(bw, nrcodes[i]);
    for (int i = 0; i < nvals; i++) bw_put_byte(bw, vals[i]);
}

static void jpeg_write_headers(jpeg_bitwriter_t *bw, const jpeg_tables_t *t,
                               int width, int height)
{
    /* SOI */
    bw_put_u16(bw, 0xFFD8U);

    /* APP0 JFIF */
    bw_put_u16(bw, 0xFFE0U);
    bw_put_u16(bw, 16);
    bw_put_byte(bw, 'J'); bw_put_byte(bw, 'F'); bw_put_byte(bw, 'I');
    bw_put_byte(bw, 'F'); bw_put_byte(bw, 0);
    bw_put_byte(bw, 1); bw_put_byte(bw, 1); /* v1.01 */
    bw_put_byte(bw, 0);                     /* no units */
    bw_put_u16(bw, 1); bw_put_u16(bw, 1);   /* 1:1 pixel aspect */
    bw_put_byte(bw, 0); bw_put_byte(bw, 0); /* no thumbnail */

    /* DQT Luma (id=0) and Chroma (id=1) combined */
    bw_put_u16(bw, 0xFFDBU);
    bw_put_u16(bw, 2 + 65 + 65);
    bw_put_byte(bw, 0x00U);
    for (int i = 0; i < 64; i++) bw_put_byte(bw, t->qt_luma_zz[i]);
    bw_put_byte(bw, 0x01U);
    for (int i = 0; i < 64; i++) bw_put_byte(bw, t->qt_chroma_zz[i]);

    /* SOF0 Baseline DCT, 3 components, 4:2:2 (Y 2x1, Cb 1x1, Cr 1x1) */
    bw_put_u16(bw, 0xFFC0U);
    bw_put_u16(bw, 8 + 3 * 3);
    bw_put_byte(bw, 8);
    bw_put_u16(bw, (uint16_t)height);
    bw_put_u16(bw, (uint16_t)width);
    bw_put_byte(bw, 3);
    bw_put_byte(bw, 1); bw_put_byte(bw, 0x21U); bw_put_byte(bw, 0); /* Y */
    bw_put_byte(bw, 2); bw_put_byte(bw, 0x11U); bw_put_byte(bw, 1); /* Cb */
    bw_put_byte(bw, 3); bw_put_byte(bw, 0x11U); bw_put_byte(bw, 1); /* Cr */

    /* DHT */
    jpeg_write_dht(bw, 0x00U, g_dc_luma_nrcodes,   g_dc_luma_vals,   12);
    jpeg_write_dht(bw, 0x10U, g_ac_luma_nrcodes,   g_ac_luma_vals,   162);
    jpeg_write_dht(bw, 0x01U, g_dc_chroma_nrcodes, g_dc_chroma_vals, 12);
    jpeg_write_dht(bw, 0x11U, g_ac_chroma_nrcodes, g_ac_chroma_vals, 162);

    /* SOS */
    bw_put_u16(bw, 0xFFDAU);
    bw_put_u16(bw, 6 + 2 * 3);
    bw_put_byte(bw, 3);
    bw_put_byte(bw, 1); bw_put_byte(bw, 0x00U);
    bw_put_byte(bw, 2); bw_put_byte(bw, 0x11U);
    bw_put_byte(bw, 3); bw_put_byte(bw, 0x11U);
    bw_put_byte(bw, 0); bw_put_byte(bw, 63); bw_put_byte(bw, 0);
}

static inline int bit_size_and_code(int val, uint16_t *bits_out)
{
    int abs_v = (val < 0) ? -val : val;
    int bits = 0;
    int t = abs_v;
    while (t > 0)
    {
        bits++;
        t >>= 1;
    }
    if (bits > 11)
    {
        bits = 11;
    }
    {
        uint32_t mask = (1U << bits) - 1U;
        uint32_t raw = (val < 0) ? (uint32_t)(val - 1) : (uint32_t)val;
        *bits_out = (uint16_t)(raw & mask);
    }
    return bits;
}

static JPEG_ITCM_SECTION void fdct_aan_2d(float data[64])
{
    float *d;
    int pass;

    /* Pass 1: rows */
    d = data;
    for (pass = 0; pass < 8; pass++, d += 8)
    {
        float tmp0 = d[0] + d[7], tmp7 = d[0] - d[7];
        float tmp1 = d[1] + d[6], tmp6 = d[1] - d[6];
        float tmp2 = d[2] + d[5], tmp5 = d[2] - d[5];
        float tmp3 = d[3] + d[4], tmp4 = d[3] - d[4];

        float tmp10 = tmp0 + tmp3, tmp13 = tmp0 - tmp3;
        float tmp11 = tmp1 + tmp2, tmp12 = tmp1 - tmp2;
        d[0] = tmp10 + tmp11;
        d[4] = tmp10 - tmp11;
        float z1 = (tmp12 + tmp13) * 0.707106781f;
        d[2] = tmp13 + z1;
        d[6] = tmp13 - z1;

        tmp10 = tmp4 + tmp5;
        tmp11 = tmp5 + tmp6;
        tmp12 = tmp6 + tmp7;
        float z5 = (tmp10 - tmp12) * 0.382683433f;
        float z2 = 0.541196100f * tmp10 + z5;
        float z4 = 1.306562965f * tmp12 + z5;
        float z3 = tmp11 * 0.707106781f;
        float z11 = tmp7 + z3, z13 = tmp7 - z3;
        d[5] = z13 + z2;
        d[3] = z13 - z2;
        d[1] = z11 + z4;
        d[7] = z11 - z4;
    }

    /* Pass 2: columns */
    d = data;
    for (pass = 0; pass < 8; pass++, d++)
    {
        float tmp0 = d[0]  + d[56], tmp7 = d[0]  - d[56];
        float tmp1 = d[8]  + d[48], tmp6 = d[8]  - d[48];
        float tmp2 = d[16] + d[40], tmp5 = d[16] - d[40];
        float tmp3 = d[24] + d[32], tmp4 = d[24] - d[32];

        float tmp10 = tmp0 + tmp3, tmp13 = tmp0 - tmp3;
        float tmp11 = tmp1 + tmp2, tmp12 = tmp1 - tmp2;
        d[0]  = tmp10 + tmp11;
        d[32] = tmp10 - tmp11;
        float z1 = (tmp12 + tmp13) * 0.707106781f;
        d[16] = tmp13 + z1;
        d[48] = tmp13 - z1;

        tmp10 = tmp4 + tmp5;
        tmp11 = tmp5 + tmp6;
        tmp12 = tmp6 + tmp7;
        float z5 = (tmp10 - tmp12) * 0.382683433f;
        float z2 = 0.541196100f * tmp10 + z5;
        float z4 = 1.306562965f * tmp12 + z5;
        float z3 = tmp11 * 0.707106781f;
        float z11 = tmp7 + z3, z13 = tmp7 - z3;
        d[40] = z13 + z2;
        d[24] = z13 - z2;
        d[8]  = z11 + z4;
        d[56] = z11 - z4;
    }
}

static JPEG_ITCM_SECTION int encode_block(jpeg_bitwriter_t *bw,
                                          float block[64],
                                          const float inv_q[64],
                                          int prev_dc,
                                          const huff_sym_t dc_tbl[12],
                                          const huff_sym_t ac_tbl[256])
{
    int qzz[64];
    int i, last_nz, zero_run;
    int dc_diff, dc_val;
    uint16_t bits_val = 0;
    int s;

    fdct_aan_2d(block);

    for (i = 0; i < 64; i++)
    {
        float v = block[g_zigzag[i]] * inv_q[g_zigzag[i]];
        qzz[i] = (v >= 0.0f) ? (int)(v + 0.5f) : (int)(v - 0.5f);
    }

    dc_val = qzz[0];
    if (dc_val > 2047) dc_val = 2047;
    if (dc_val < -2047) dc_val = -2047;
    dc_diff = dc_val - prev_dc;
    if (dc_diff > 2047) dc_diff = 2047;
    if (dc_diff < -2047) dc_diff = -2047;
    if (dc_diff == 0)
    {
        bw_put_bits(bw, dc_tbl[0].code, dc_tbl[0].len);
    }
    else
    {
        s = bit_size_and_code(dc_diff, &bits_val);
        bw_put_bits(bw, dc_tbl[s].code, dc_tbl[s].len);
        bw_put_bits(bw, bits_val, s);
    }

    last_nz = 63;
    while (last_nz > 0 && qzz[last_nz] == 0)
    {
        last_nz--;
    }

    if (last_nz == 0)
    {
        bw_put_bits(bw, ac_tbl[0x00].code, ac_tbl[0x00].len); /* EOB */
        return dc_val;
    }

    zero_run = 0;
    for (i = 1; i <= last_nz; i++)
    {
        int coeff = qzz[i];
        if (coeff == 0)
        {
            zero_run++;
        }
        else
        {
            while (zero_run >= 16)
            {
                bw_put_bits(bw, ac_tbl[0xF0].code, ac_tbl[0xF0].len); /* ZRL */
                zero_run -= 16;
            }
            if (coeff > 1023) coeff = 1023;
            if (coeff < -1023) coeff = -1023;
            s = bit_size_and_code(coeff, &bits_val);
            uint8_t rrrrssss = (uint8_t)((zero_run << 4) | s);
            bw_put_bits(bw, ac_tbl[rrrrssss].code, ac_tbl[rrrrssss].len);
            bw_put_bits(bw, bits_val, s);
            zero_run = 0;
        }
    }

    if (last_nz < 63)
    {
        bw_put_bits(bw, ac_tbl[0x00].code, ac_tbl[0x00].len); /* EOB */
    }

    return dc_val;
}

JPEG_ITCM_SECTION size_t mini_jpeg_encode_yuyv(const uint8_t *yuyv, int width, int height,
                                               int quality, uint8_t *out_buf, size_t out_cap)
{
    const jpeg_tables_t *tbl;
    jpeg_bitwriter_t bw;
    float y0_blk[64], y1_blk[64], cb_blk[64], cr_blk[64];
    int dc_y = 0, dc_cb = 0, dc_cr = 0;
    int mx, my;

    if ((yuyv == NULL) || (out_buf == NULL) || (out_cap < 1024U) ||
        (width < 8) || (height < 8) || (width & 1))
    {
        return 0U;
    }

    tbl = jpeg_acquire_tables(quality);
    memset(&bw, 0, sizeof(bw));
    bw.buf = out_buf;
    bw.cap = out_cap;

    jpeg_write_headers(&bw, tbl, width, height);

    for (my = 0; my < height; my += 8)
    {
        for (mx = 0; mx < width; mx += 16)
        {
            int r, p;
            for (r = 0; r < 8; r++)
            {
                int sy = my + r;
                if (sy >= height) sy = height - 1;
                const uint8_t *row = yuyv + (size_t)sy * (size_t)width * 2U;

                for (p = 0; p < 8; p++)
                {
                    int sx = mx + p * 2;
                    if (sx + 1 >= width) sx = width - 2;
                    const uint8_t *px = row + (size_t)sx * 2U;
                    int dst_y = r * 8;

                    float cb_v = (float)px[1] - 128.0f;
                    float cr_v = (float)px[3] - 128.0f;
                    if (p < 4)
                    {
                        y0_blk[dst_y + p * 2]     = (float)px[0] - 128.0f;
                        y0_blk[dst_y + p * 2 + 1] = (float)px[2] - 128.0f;
                    }
                    else
                    {
                        int q = p - 4;
                        y1_blk[dst_y + q * 2]     = (float)px[0] - 128.0f;
                        y1_blk[dst_y + q * 2 + 1] = (float)px[2] - 128.0f;
                    }
                    cb_blk[dst_y + p] = cb_v;
                    cr_blk[dst_y + p] = cr_v;
                }
            }

            dc_y  = encode_block(&bw, y0_blk, tbl->inv_luma,   dc_y,  tbl->dc_luma,   tbl->ac_luma);
            dc_y  = encode_block(&bw, y1_blk, tbl->inv_luma,   dc_y,  tbl->dc_luma,   tbl->ac_luma);
            dc_cb = encode_block(&bw, cb_blk, tbl->inv_chroma, dc_cb, tbl->dc_chroma, tbl->ac_chroma);
            dc_cr = encode_block(&bw, cr_blk, tbl->inv_chroma, dc_cr, tbl->dc_chroma, tbl->ac_chroma);

            if (bw.overflow)
            {
                jpeg_release_tables();
                return 0U;
            }
        }
    }

    bw_flush_bits(&bw);
    bw_put_u16(&bw, 0xFFD9U); /* EOI */
    jpeg_release_tables();
    return bw.overflow ? 0U : bw.pos;
}

JPEG_ITCM_SECTION size_t mini_jpeg_encode_rgb565(const uint16_t *rgb565, int width, int height,
                                                 int quality, uint8_t *out_buf, size_t out_cap)
{
    const jpeg_tables_t *tbl;
    jpeg_bitwriter_t bw;
    float y0_blk[64], y1_blk[64], cb_blk[64], cr_blk[64];
    int dc_y = 0, dc_cb = 0, dc_cr = 0;
    int mx, my;

    if ((rgb565 == NULL) || (out_buf == NULL) || (out_cap < 1024U) ||
        (width < 8) || (height < 8) || (width & 1))
    {
        return 0U;
    }

    tbl = jpeg_acquire_tables(quality);
    memset(&bw, 0, sizeof(bw));
    bw.buf = out_buf;
    bw.cap = out_cap;

    jpeg_write_headers(&bw, tbl, width, height);

    for (my = 0; my < height; my += 8)
    {
        for (mx = 0; mx < width; mx += 16)
        {
            int r, p;
            for (r = 0; r < 8; r++)
            {
                int sy = my + r;
                if (sy >= height) sy = height - 1;
                const uint16_t *row = rgb565 + (size_t)sy * (size_t)width;

                for (p = 0; p < 8; p++)
                {
                    int sx = mx + p * 2;
                    if (sx + 1 >= width) sx = width - 2;

                    uint16_t c0 = row[sx];
                    uint16_t c1 = row[sx + 1];

                    int r0 = (int)((c0 >> 8) & 0xF8U) | ((c0 >> 13) & 0x07U);
                    int g0 = (int)((c0 >> 3) & 0xFCU) | ((c0 >> 9)  & 0x03U);
                    int b0 = (int)((c0 << 3) & 0xF8U) | ((c0 >> 2)  & 0x07U);

                    int r1 = (int)((c1 >> 8) & 0xF8U) | ((c1 >> 13) & 0x07U);
                    int g1 = (int)((c1 >> 3) & 0xFCU) | ((c1 >> 9)  & 0x03U);
                    int b1 = (int)((c1 << 3) & 0xF8U) | ((c1 >> 2)  & 0x07U);

                    /* BT.601 full-range YCbCr centered around 0 */
                    float y0 =  0.299f * r0 + 0.587f * g0 + 0.114f * b0 - 128.0f;
                    float y1 =  0.299f * r1 + 0.587f * g1 + 0.114f * b1 - 128.0f;

                    int ra = (r0 + r1) >> 1;
                    int ga = (g0 + g1) >> 1;
                    int ba = (b0 + b1) >> 1;
                    float cb_v = -0.168736f * ra - 0.331264f * ga + 0.5f * ba;
                    float cr_v =  0.5f * ra - 0.418688f * ga - 0.081312f * ba;

                    int dst_y = r * 8;
                    if (p < 4)
                    {
                        y0_blk[dst_y + p * 2]     = y0;
                        y0_blk[dst_y + p * 2 + 1] = y1;
                    }
                    else
                    {
                        int q = p - 4;
                        y1_blk[dst_y + q * 2]     = y0;
                        y1_blk[dst_y + q * 2 + 1] = y1;
                    }
                    cb_blk[dst_y + p] = cb_v;
                    cr_blk[dst_y + p] = cr_v;
                }
            }

            dc_y  = encode_block(&bw, y0_blk, tbl->inv_luma,   dc_y,  tbl->dc_luma,   tbl->ac_luma);
            dc_y  = encode_block(&bw, y1_blk, tbl->inv_luma,   dc_y,  tbl->dc_luma,   tbl->ac_luma);
            dc_cb = encode_block(&bw, cb_blk, tbl->inv_chroma, dc_cb, tbl->dc_chroma, tbl->ac_chroma);
            dc_cr = encode_block(&bw, cr_blk, tbl->inv_chroma, dc_cr, tbl->dc_chroma, tbl->ac_chroma);

            if (bw.overflow)
            {
                jpeg_release_tables();
                return 0U;
            }
        }
    }

    bw_flush_bits(&bw);
    bw_put_u16(&bw, 0xFFD9U); /* EOI */
    jpeg_release_tables();
    return bw.overflow ? 0U : bw.pos;
}
