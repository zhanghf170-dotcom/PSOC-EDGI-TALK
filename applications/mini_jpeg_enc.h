/*
 * Lightweight, reentrant baseline JPEG encoder (YUYV 4:2:2 & RGB565 4:2:2)
 * Zero heap allocation — uses caller-supplied output buffer.
 */

#ifndef MINI_JPEG_ENC_H__
#define MINI_JPEG_ENC_H__

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Encode YUYV (4:2:2 packed: Y0 U Y1 V) frame into Baseline JFIF JPEG.
 *
 * @param yuyv     Source YUYV pixel buffer (width * height * 2 bytes)
 * @param width    Image width in pixels (must be >= 8 and even; best multiple of 16)
 * @param height   Image height in pixels (must be >= 8)
 * @param quality  JPEG quality [1..100], recommended 50..85
 * @param out_buf  Destination buffer for JPEG bytes
 * @param out_cap  Capacity of out_buf in bytes
 * @return Encoded JPEG size in bytes, or 0 on error/overflow.
 */
size_t mini_jpeg_encode_yuyv(const uint8_t *yuyv, int width, int height,
                             int quality, uint8_t *out_buf, size_t out_cap);

/**
 * Encode RGB565 (little-endian uint16_t) frame into Baseline JFIF JPEG (4:2:2).
 */
size_t mini_jpeg_encode_rgb565(const uint16_t *rgb565, int width, int height,
                               int quality, uint8_t *out_buf, size_t out_cap);

#ifdef __cplusplus
}
#endif

#endif /* MINI_JPEG_ENC_H__ */
