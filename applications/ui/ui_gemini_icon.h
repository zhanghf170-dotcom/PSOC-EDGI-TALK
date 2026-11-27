#ifndef UI_GEMINI_ICON_H__
#define UI_GEMINI_ICON_H__

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Built-in Flash (.rodata) simplified Gemini-chan & Gemini star icons.
 * VG-Lite safe:
 *   - Widths (48, 32) are multiples of 16 px (gcFEATURE_VG_16PIXELS_ALIGNED)
 *   - Strides (192 B, 128 B) are multiples of 64 B
 *   - Pixel buffers are 64-byte aligned (LV_DRAW_BUF_ALIGN == 64)
 *   - Premultiplied ARGB8888 (BGRA byte order) with LV_IMAGE_FLAGS_PREMULTIPLIED
 *     so LVGL/VG-Lite renders directly from Flash with zero RAM duplication.
 */
extern const lv_image_dsc_t g_ui_gemini_icon_48;
extern const lv_image_dsc_t g_ui_gemini_icon_32;

#ifdef __cplusplus
}
#endif

#endif /* UI_GEMINI_ICON_H__ */
