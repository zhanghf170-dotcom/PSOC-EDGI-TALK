#include <rtthread.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#include "uvc_ai_app.h"
#include "uvc_ai.h"
#include "usbh_uvc_display_hook.h"
#include "usb_config.h"
#include "alert_manager.h"

/* AHT20 sensor globals from main.c */
extern float g_aht_temp;
extern float g_aht_humi;

#undef USB_DBG_TAG
#define USB_DBG_TAG "uvc_ai_app"
#include "usb_log.h"

#define UVC_RGB565(r, g, b) (uint16_t)((((uint16_t)(r) & 0xF8U) << 8) | (((uint16_t)(g) & 0xFCU) << 3) | ((uint16_t)(b) >> 3))

#define UVC_AI_BOX_THICKNESS       2U
#define UVC_AI_FONT_WIDTH          5U
#define UVC_AI_FONT_HEIGHT         7U
#define UVC_AI_FONT_SPACING        1U
#define UVC_AI_TEXT_PADDING_X      2U
#define UVC_AI_TEXT_PADDING_Y      2U
#define UVC_AI_LABEL_MAX_LEN       32U
#define UVC_AI_FRAME_BYTES         ((uint32_t)UVC_AI_CAMERA_WIDTH * UVC_AI_CAMERA_HEIGHT * 2U)
#define UVC_AI_WORKER_STACK_SIZE   65536U
#define UVC_AI_STACK_SECTION       __attribute__((section(".cy_ai_worker_stack")))
/* Let the LVGL thread finish short refresh work before CPU-side inference. */
#define UVC_AI_WORKER_PRIORITY     16U
#define UVC_AI_WORKER_TICK         5U
#define UVC_AI_WORKER_NAME         "uvc_aiwk"
#define UVC_AI_RESULT_MAX_AGE_MS   500

typedef struct {
    char ch;
    uint8_t rows[UVC_AI_FONT_HEIGHT];
} uvc_font5x7_t;

typedef struct {
    rt_bool_t started;
    rt_bool_t ai_ready;
    volatile rt_bool_t worker_running;
    volatile rt_bool_t worker_busy;
    uint32_t frame_counter;
    uint16_t src_width;
    uint16_t src_height;
    uvc_ai_result_t latest_result;
    rt_tick_t result_tick;
    uint32_t result_seq;
} uvc_ai_app_ctx_t;

static uvc_ai_app_ctx_t g_uvc_ai_app;
static struct rt_mutex g_uvc_ai_lifecycle;
/* Place 64KB AI worker stack in NOLOAD D-Cacheable SoCMEM (frees 64KB DTCM + 64KB Flash). */
static UVC_AI_STACK_SECTION rt_uint8_t g_uvc_ai_worker_stack[UVC_AI_WORKER_STACK_SIZE];
static struct rt_thread g_uvc_ai_worker_tcb;
static struct rt_semaphore g_uvc_ai_worker_wake;
static rt_bool_t g_uvc_ai_worker_initialized;
static __attribute__((section(".cy_ml_arena_fast_data"), aligned(32)))
    uint8_t g_uvc_ai_worker_frame[UVC_AI_FRAME_BYTES];
static uvc_ai_result_t g_previous_ball_result;
static rt_tick_t g_previous_ball_tick;

static const uint16_t g_uvc_ai_class_colors[UVC_AI_NUM_CLASSES] = {
    UVC_RGB565(227, 66, 24),  /* class 0 */
    UVC_RGB565(8, 24, 168),   /* class 1 */
    UVC_RGB565(0, 120, 0),    /* class 2 */
};
static const uint16_t g_uvc_ai_unknown_color = UVC_RGB565(255, 215, 0);
static const uint16_t g_uvc_ai_text_color = UVC_RGB565(255, 255, 255);
static const uint16_t g_uvc_ai_model_bg = UVC_RGB565(0, 0, 0);

static int uvc_ai_app_init_lock(void)
{
    return rt_mutex_init(&g_uvc_ai_lifecycle, "ai_life", RT_IPC_FLAG_PRIO);
}
INIT_COMPONENT_EXPORT(uvc_ai_app_init_lock);

/* ---- Closed Eye risk-level classification ---- */
#define UVC_AI_RISK_HIGH_THRESHOLD    0.90f   /* >= 90%: High risk */
#define UVC_AI_RISK_MID_THRESHOLD     0.80f   /* >= 80%: Mid risk */
#define UVC_AI_RISK_LOW_THRESHOLD     0.70f   /* >= 70%: Low risk, below: ignore */

/* Risk level enum */
#define UVC_AI_RISK_LOW    0U
#define UVC_AI_RISK_MID    1U
#define UVC_AI_RISK_HIGH   2U

static const uint16_t g_uvc_ai_risk_colors[3] = {
    UVC_RGB565(0,   200, 0),     /* Low  risk: Green  */
    UVC_RGB565(255, 165, 0),     /* Mid  risk: Orange */
    UVC_RGB565(255, 0,   0),     /* High risk: Red    */
};

static const char *g_uvc_ai_risk_labels[3] = {
    "Low Risk",
    "Mid Risk",
    "High Risk",
};

static inline uint8_t uvc_ai_get_risk_level(float closed_eye_conf)
{
    if (closed_eye_conf >= UVC_AI_RISK_HIGH_THRESHOLD) {
        return UVC_AI_RISK_HIGH;
    }
    if (closed_eye_conf >= UVC_AI_RISK_MID_THRESHOLD) {
        return UVC_AI_RISK_MID;
    }
    return UVC_AI_RISK_LOW;
}

static const uvc_font5x7_t g_uvc_font5x7[] = {
    {' ', {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
    {'%', {0x19, 0x19, 0x02, 0x04, 0x08, 0x13, 0x13}},
    {'-', {0x00, 0x00, 0x00, 0x1f, 0x00, 0x00, 0x00}},
    {'.', {0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x04}},
    {':', {0x00, 0x04, 0x04, 0x00, 0x04, 0x04, 0x00}},
    {'?', {0x0e, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04}},
    {'0', {0x0e, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0e}},
    {'1', {0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e}},
    {'2', {0x0e, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1f}},
    {'3', {0x1e, 0x01, 0x01, 0x0e, 0x01, 0x01, 0x1e}},
    {'4', {0x02, 0x06, 0x0a, 0x12, 0x1f, 0x02, 0x02}},
    {'5', {0x1f, 0x10, 0x1e, 0x01, 0x01, 0x11, 0x0e}},
    {'6', {0x06, 0x08, 0x10, 0x1e, 0x11, 0x11, 0x0e}},
    {'7', {0x1f, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
    {'8', {0x0e, 0x11, 0x11, 0x0e, 0x11, 0x11, 0x0e}},
    {'9', {0x0e, 0x11, 0x11, 0x0f, 0x01, 0x02, 0x0c}},
    {'A', {0x0e, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11}},
    {'B', {0x1e, 0x11, 0x11, 0x1e, 0x11, 0x11, 0x1e}},
    {'C', {0x0e, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0e}},
    {'D', {0x1c, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1c}},
    {'E', {0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x1f}},
    {'F', {0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x10}},
    {'G', {0x0e, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0e}},
    {'H', {0x11, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11}},
    {'I', {0x0e, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0e}},
    {'J', {0x01, 0x01, 0x01, 0x01, 0x11, 0x11, 0x0e}},
    {'K', {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}},
    {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1f}},
    {'M', {0x11, 0x1b, 0x15, 0x15, 0x11, 0x11, 0x11}},
    {'N', {0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11}},
    {'O', {0x0e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e}},
    {'P', {0x1e, 0x11, 0x11, 0x1e, 0x10, 0x10, 0x10}},
    {'Q', {0x0e, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0d}},
    {'R', {0x1e, 0x11, 0x11, 0x1e, 0x14, 0x12, 0x11}},
    {'S', {0x0f, 0x10, 0x10, 0x0e, 0x01, 0x01, 0x1e}},
    {'T', {0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
    {'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e}},
    {'V', {0x11, 0x11, 0x11, 0x11, 0x11, 0x0a, 0x04}},
    {'W', {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0a}},
    {'X', {0x11, 0x11, 0x0a, 0x04, 0x0a, 0x11, 0x11}},
    {'Y', {0x11, 0x11, 0x0a, 0x04, 0x04, 0x04, 0x04}},
    {'Z', {0x1f, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1f}},
};

static inline int16_t uvc_ai_app_clamp_i16(int32_t value, int32_t min_v, int32_t max_v)
{
    if (value < min_v) {
        value = min_v;
    }

    if (value > max_v) {
        value = max_v;
    }

    return (int16_t)value;
}

static void uvc_ai_app_publish_result(const uvc_ai_result_t *result)
{
    rt_base_t level;

    if (result == RT_NULL) {
        return;
    }

    level = rt_hw_interrupt_disable();
    memcpy(&g_uvc_ai_app.latest_result, result, sizeof(g_uvc_ai_app.latest_result));
    g_uvc_ai_app.result_tick = rt_tick_get();
    g_uvc_ai_app.result_seq++;
    rt_hw_interrupt_enable(level);
}

static uint32_t uvc_ai_app_snapshot_result(uvc_ai_result_t *result, rt_tick_t *tick)
{
    rt_base_t level = rt_hw_interrupt_disable();
    memcpy(result, &g_uvc_ai_app.latest_result, sizeof(*result));
    *tick = g_uvc_ai_app.result_tick;
    uint32_t seq = g_uvc_ai_app.result_seq;
    rt_hw_interrupt_enable(level);
    return seq;
}

static inline char uvc_ai_app_to_upper_ascii(char ch)
{
    if ((ch >= 'a') && (ch <= 'z')) {
        return (char)(ch - ('a' - 'A'));
    }

    return ch;
}

static const uint8_t *uvc_ai_app_get_glyph5x7(char ch)
{
    uint32_t i;
    uint32_t glyph_count = sizeof(g_uvc_font5x7) / sizeof(g_uvc_font5x7[0]);
    char upper = uvc_ai_app_to_upper_ascii(ch);

    for (i = 0U; i < glyph_count; i++) {
        if (g_uvc_font5x7[i].ch == upper) {
            return g_uvc_font5x7[i].rows;
        }
    }

    return g_uvc_font5x7[5].rows; /* '?' */
}

static void uvc_ai_app_fill_rect(const uvc_display_overlay_info_t *info,
                                 int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                                 uint16_t color)
{
    int16_t x_start = x0;
    int16_t y_start = y0;
    int16_t x_end = x1;
    int16_t y_end = y1;
    int16_t y;

    if ((info == RT_NULL) || (info->framebuffer == RT_NULL)) {
        return;
    }

    if (x_start > x_end) {
        int16_t tmp = x_start;
        x_start = x_end;
        x_end = tmp;
    }

    if (y_start > y_end) {
        int16_t tmp = y_start;
        y_start = y_end;
        y_end = tmp;
    }

    x_start = uvc_ai_app_clamp_i16(x_start, 0, (int32_t)info->lcd_width - 1);
    y_start = uvc_ai_app_clamp_i16(y_start, 0, (int32_t)info->lcd_height - 1);
    x_end = uvc_ai_app_clamp_i16(x_end, 0, (int32_t)info->lcd_width - 1);
    y_end = uvc_ai_app_clamp_i16(y_end, 0, (int32_t)info->lcd_height - 1);
    if ((x_start > x_end) || (y_start > y_end)) {
        return;
    }

    for (y = y_start; y <= y_end; y++) {
        uint16_t *row = info->framebuffer + (uint32_t)y * info->framebuffer_stride + (uint16_t)x_start;
        int16_t x;

        for (x = x_start; x <= x_end; x++) {
            *row++ = color;
        }
    }
}

static void uvc_ai_app_draw_hline(const uvc_display_overlay_info_t *info,
                                  int16_t x0, int16_t x1, int16_t y, uint16_t color)
{
    int16_t x_start = x0;
    int16_t x_end = x1;
    int16_t x;
    uint16_t *row;

    if ((info == RT_NULL) || (info->framebuffer == RT_NULL) ||
        (y < 0) || (y >= info->lcd_height)) {
        return;
    }

    if (x_start > x_end) {
        int16_t tmp = x_start;
        x_start = x_end;
        x_end = tmp;
    }

    x_start = uvc_ai_app_clamp_i16(x_start, 0, (int32_t)info->lcd_width - 1);
    x_end = uvc_ai_app_clamp_i16(x_end, 0, (int32_t)info->lcd_width - 1);
    if (x_start > x_end) {
        return;
    }

    row = info->framebuffer + (uint32_t)y * info->framebuffer_stride + (uint16_t)x_start;
    for (x = x_start; x <= x_end; x++) {
        *row++ = color;
    }
}

static void uvc_ai_app_draw_vline(const uvc_display_overlay_info_t *info,
                                  int16_t x, int16_t y0, int16_t y1, uint16_t color)
{
    int16_t y_start = y0;
    int16_t y_end = y1;
    int16_t y;

    if ((info == RT_NULL) || (info->framebuffer == RT_NULL) ||
        (x < 0) || (x >= info->lcd_width)) {
        return;
    }

    if (y_start > y_end) {
        int16_t tmp = y_start;
        y_start = y_end;
        y_end = tmp;
    }

    y_start = uvc_ai_app_clamp_i16(y_start, 0, (int32_t)info->lcd_height - 1);
    y_end = uvc_ai_app_clamp_i16(y_end, 0, (int32_t)info->lcd_height - 1);
    if (y_start > y_end) {
        return;
    }

    for (y = y_start; y <= y_end; y++) {
        info->framebuffer[(uint32_t)y * info->framebuffer_stride + (uint16_t)x] = color;
    }
}

static void uvc_ai_app_draw_rect(const uvc_display_overlay_info_t *info,
                                 int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                                 uint16_t color, uint8_t thickness)
{
    uint8_t i;
    int16_t x_start = x0;
    int16_t y_start = y0;
    int16_t x_end = x1;
    int16_t y_end = y1;

    if (x_start > x_end) {
        int16_t tmp = x_start;
        x_start = x_end;
        x_end = tmp;
    }
    if (y_start > y_end) {
        int16_t tmp = y_start;
        y_start = y_end;
        y_end = tmp;
    }

    for (i = 0U; i < thickness; i++) {
        int16_t left = (int16_t)(x_start + i);
        int16_t right = (int16_t)(x_end - i);
        int16_t top = (int16_t)(y_start + i);
        int16_t bottom = (int16_t)(y_end - i);

        if ((left > right) || (top > bottom)) {
            break;
        }

        uvc_ai_app_draw_hline(info, left, right, top, color);
        uvc_ai_app_draw_hline(info, left, right, bottom, color);
        uvc_ai_app_draw_vline(info, left, top, bottom, color);
        uvc_ai_app_draw_vline(info, right, top, bottom, color);
    }
}

static void uvc_ai_app_draw_char5x7(const uvc_display_overlay_info_t *info,
                                    int16_t x, int16_t y, char ch, uint16_t color)
{
    const uint8_t *rows = uvc_ai_app_get_glyph5x7(ch);
    uint32_t row;

    if ((info == RT_NULL) || (info->framebuffer == RT_NULL)) {
        return;
    }

    for (row = 0U; row < UVC_AI_FONT_HEIGHT; row++) {
        int16_t py = (int16_t)(y + (int16_t)row);
        uint16_t *dst_row;
        uint8_t bits = rows[row];
        uint32_t col;

        if ((py < 0) || (py >= info->lcd_height)) {
            continue;
        }

        dst_row = info->framebuffer + (uint32_t)py * info->framebuffer_stride;
        for (col = 0U; col < UVC_AI_FONT_WIDTH; col++) {
            if ((bits & (uint8_t)(1U << (UVC_AI_FONT_WIDTH - 1U - col))) != 0U) {
                int16_t px = (int16_t)(x + (int16_t)col);

                if ((px >= 0) && (px < info->lcd_width)) {
                    dst_row[px] = color;
                }
            }
        }
    }
}

static void uvc_ai_app_draw_text5x7(const uvc_display_overlay_info_t *info,
                                    int16_t x, int16_t y, const char *text, uint16_t color)
{
    int16_t cursor_x = x;

    if (text == RT_NULL) {
        return;
    }

    while (*text != '\0') {
        uvc_ai_app_draw_char5x7(info, cursor_x, y, *text, color);
        cursor_x = (int16_t)(cursor_x + UVC_AI_FONT_WIDTH + UVC_AI_FONT_SPACING);
        text++;
    }
}

static void uvc_ai_app_draw_text_scaled(const uvc_display_overlay_info_t *info,
                                        int16_t x, int16_t y, const char *text,
                                        uint16_t color, int scale)
{
    int16_t cx = x;
    int16_t char_step = (int16_t)((UVC_AI_FONT_WIDTH + UVC_AI_FONT_SPACING) * scale);

    if ((info == RT_NULL) || (info->framebuffer == RT_NULL) || (text == RT_NULL) || (scale <= 0))
    {
        return;
    }

    while (*text)
    {
        const uint8_t *rows = uvc_ai_app_get_glyph5x7(*text);
        uint32_t row;

        for (row = 0U; row < UVC_AI_FONT_HEIGHT; row++)
        {
            uint8_t bits = rows[row];
            uint32_t col;
            for (col = 0U; col < UVC_AI_FONT_WIDTH; col++)
            {
                if ((bits & (uint8_t)(1U << (UVC_AI_FONT_WIDTH - 1U - col))) != 0U)
                {
                    int16_t bx = (int16_t)(cx + (int16_t)(col * (uint32_t)scale));
                    int16_t by = (int16_t)(y + (int16_t)(row * (uint32_t)scale));
                    int ry, rx;
                    for (ry = 0; ry < scale; ry++)
                    {
                        int16_t py = (int16_t)(by + ry);
                        uint16_t *row_ptr;
                        if (py < 0 || py >= info->lcd_height) continue;
                        row_ptr = info->framebuffer + (uint32_t)py * info->framebuffer_stride;
                        for (rx = 0; rx < scale; rx++)
                        {
                            int16_t px = (int16_t)(bx + rx);
                            if (px >= 0 && px < info->lcd_width)
                                row_ptr[px] = color;
                        }
                    }
                }
            }
        }
        cx = (int16_t)(cx + char_step);
        text++;
    }
}

static uint16_t uvc_ai_app_text_width5x7(const char *text)
{
    uint32_t len;

    if ((text == RT_NULL) || (*text == '\0')) {
        return 0U;
    }

    len = (uint32_t)strlen(text);
    return (uint16_t)(len * (UVC_AI_FONT_WIDTH + UVC_AI_FONT_SPACING) - UVC_AI_FONT_SPACING);
}

static int16_t uvc_ai_app_map_x_to_lcd(const uvc_display_overlay_info_t *info, int16_t ai_x)
{
    int32_t x;

    if ((info == RT_NULL) || (info->src_width == 0U) || (info->dst_width == 0U)) {
        return 0;
    }

    x = uvc_ai_app_clamp_i16(ai_x, 0, (int32_t)UVC_AI_IMAGE_WIDTH - 1);
    x = (x * (int32_t)info->src_width) / (int32_t)UVC_AI_IMAGE_WIDTH;
    x = (x * (int32_t)info->dst_width) / (int32_t)info->src_width;

    return uvc_ai_app_clamp_i16(x, 0, (int32_t)info->lcd_width - 1);
}

static int16_t uvc_ai_app_map_y_to_lcd(const uvc_display_overlay_info_t *info, int16_t ai_y)
{
    int32_t y;

    if ((info == RT_NULL) || (info->src_height == 0U) || (info->dst_height == 0U)) {
        return 0;
    }

    y = uvc_ai_app_clamp_i16(ai_y, 0, (int32_t)UVC_AI_IMAGE_HEIGHT - 1);
    y = (y * (int32_t)info->src_height) / (int32_t)UVC_AI_IMAGE_HEIGHT;
    y = (y * (int32_t)info->dst_height) / (int32_t)info->src_height + (int32_t)info->dst_y_offset;

    return uvc_ai_app_clamp_i16(y, 0, (int32_t)info->lcd_height - 1);
}

static int16_t uvc_ai_app_map_cam_y_to_lcd(const uvc_display_overlay_info_t *info, int16_t cam_y)
{
    int32_t y;

    if ((info == RT_NULL) || (info->dst_height == 0U)) {
        return 0;
    }

    y = uvc_ai_app_clamp_i16(cam_y, 0, (int32_t)UVC_AI_CAMERA_HEIGHT - 1);
    y = (y * (int32_t)info->dst_height) / (int32_t)UVC_AI_CAMERA_HEIGHT + (int32_t)info->dst_y_offset;

    return uvc_ai_app_clamp_i16(y, 0, (int32_t)info->lcd_height - 1);
}

static void uvc_ai_app_draw_text_box(const uvc_display_overlay_info_t *info,
                                     int16_t x, int16_t y, const char *text,
                                     uint16_t bg_color, uint16_t fg_color)
{
    uint16_t text_w = uvc_ai_app_text_width5x7(text);
    uint16_t box_w;
    uint16_t box_h;
    int16_t x0;
    int16_t y0;
    int16_t x1;
    int16_t y1;
    int16_t min_y;
    int16_t max_y;

    if ((info == RT_NULL) || (text_w == 0U)) {
        return;
    }

    box_w = (uint16_t)(text_w + UVC_AI_TEXT_PADDING_X * 2U);
    box_h = (uint16_t)(UVC_AI_FONT_HEIGHT + UVC_AI_TEXT_PADDING_Y * 2U);

    x0 = x;
    y0 = y;

    if ((int32_t)x0 + (int32_t)box_w > info->lcd_width) {
        x0 = (int16_t)(info->lcd_width - box_w);
    }
    if (x0 < 0) {
        x0 = 0;
    }

    min_y = (int16_t)info->dst_y_offset;
    max_y = (int16_t)(info->dst_y_offset + info->dst_height);
    if (max_y > info->lcd_height) {
        max_y = info->lcd_height;
    }
    max_y = (int16_t)(max_y - box_h);

    if (y0 < min_y) {
        y0 = min_y;
    }
    if (y0 > max_y) {
        y0 = max_y;
    }

    x1 = (int16_t)(x0 + box_w - 1);
    y1 = (int16_t)(y0 + box_h - 1);

    uvc_ai_app_fill_rect(info, x0, y0, x1, y1, bg_color);
    uvc_ai_app_draw_text5x7(info,
                            (int16_t)(x0 + UVC_AI_TEXT_PADDING_X),
                            (int16_t)(y0 + UVC_AI_TEXT_PADDING_Y),
                            text,
                            fg_color);
}

static rt_bool_t uvc_ai_ball_matches(const int16_t *box, const int16_t *previous)
{
    int16_t left = box[0] > previous[0] ? box[0] : previous[0];
    int16_t top = box[1] > previous[1] ? box[1] : previous[1];
    int16_t right = box[2] < previous[2] ? box[2] : previous[2];
    int16_t bottom = box[3] < previous[3] ? box[3] : previous[3];
    if (right <= left || bottom <= top)
        return RT_FALSE;

    int32_t overlap = (right - left) * (bottom - top);
    int32_t area = (box[2] - box[0]) * (box[3] - box[1]);
    int32_t previous_area = (previous[2] - previous[0]) * (previous[3] - previous[1]);
    return 10 * overlap >= 3 * (area + previous_area - overlap);
}

static void uvc_ai_confirm_balls(uvc_ai_result_t *result)
{
    uvc_ai_result_t raw = *result;
    rt_tick_t now = rt_tick_get();
    result->count = 0U;

    if ((rt_tick_t)(now - g_previous_ball_tick) <
        rt_tick_from_millisecond(UVC_AI_RESULT_MAX_AGE_MS)) {
        for (uint8_t i = 0U; i < raw.count; i++) {
            const int16_t *box = &raw.bbox_int16[i * 4U];
            for (uint8_t j = 0U; j < g_previous_ball_result.count; j++) {
                if (!uvc_ai_ball_matches(box, &g_previous_ball_result.bbox_int16[j * 4U]))
                    continue;
                uint8_t dst = result->count++;
                memcpy(&result->bbox_int16[dst * 4U], box, 4U * sizeof(*box));
                memcpy(result->class_string[dst], raw.class_string[i], UVC_AI_MAX_CLASS_LEN);
                result->class_id[dst] = raw.class_id[i];
                result->conf[dst] = raw.conf[i];
                break;
            }
        }
    }
    g_previous_ball_result = raw;
    g_previous_ball_tick = now;
}

static void uvc_ai_app_worker_thread_entry(void *parameter)
{
    uvc_ai_result_t result;
    int ret;

    (void)parameter;

    while (1) {
        if (rt_sem_take(&g_uvc_ai_worker_wake, RT_WAITING_FOREVER) != RT_EOK) {
            continue;
        }

        if (!g_uvc_ai_app.worker_running || !g_uvc_ai_app.ai_ready) {
            g_uvc_ai_app.worker_busy = RT_FALSE;
            continue;
        }

        ret = uvc_ai_process_yuyv(g_uvc_ai_worker_frame,
                                  UVC_AI_FRAME_BYTES,
                                  &result);
        if (g_uvc_ai_app.worker_running) {
            if (ret != RT_EOK) {
                USB_LOG_WRN("AI processing failed\r\n");
                uvc_ai_reset_result(&result);
            }
            if (uvc_ai_get_mode() == UVC_AI_MODE_STEEL_BALL)
                uvc_ai_confirm_balls(&result);
            uvc_ai_app_publish_result(&result);
        }

        g_uvc_ai_app.worker_busy = RT_FALSE;
    }
}

/* Alert flash: show red warning for 2 seconds after danger trigger */
static rt_tick_t  g_alert_flash_end = 0;

/* Persistence filter: closed eye must be continuously detected for g_persist_target_ms */
static rt_tick_t  g_closed_eye_start_tick = 0;
static rt_bool_t  g_alert_sent_this_cycle = RT_FALSE;
static rt_bool_t  g_closed_eye_tracking;
static rt_tick_t  g_last_closed_eye_tick;
static uint32_t   g_last_fatigue_seq;
static uint32_t   g_persist_target_ms = 1500U;

/* Shared telemetry snapshot for LVGL UI, HTTP /api/status, and CM33 IPC */
static uvc_ai_telemetry_t g_ai_telemetry;

static void uvc_ai_app_reset_tracking(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    g_alert_flash_end = 0;
    g_closed_eye_tracking = RT_FALSE;
    g_alert_sent_this_cycle = RT_FALSE;
    rt_memset(&g_ai_telemetry, 0, sizeof(g_ai_telemetry));
    uvc_ai_reset_result(&g_uvc_ai_app.latest_result);
    uvc_ai_reset_result(&g_previous_ball_result);
    rt_hw_interrupt_enable(level);
}

static rt_bool_t uvc_ai_alert_visible(uvc_ai_mode_t mode, rt_tick_t now)
{
    return mode == UVC_AI_MODE_FATIGUE &&
           g_alert_flash_end != 0U && (rt_int32_t)(g_alert_flash_end - now) > 0;
}

void uvc_ai_app_set_persist_ms(uint32_t ms)
{
    if (ms < 500U)  ms = 500U;
    if (ms > 5000U) ms = 5000U;
    g_persist_target_ms = ms;
}

uint32_t uvc_ai_app_get_persist_ms(void)
{
    return g_persist_target_ms;
}

void uvc_ai_app_get_telemetry(uvc_ai_telemetry_t *out)
{
    rt_base_t level;
    if (out == RT_NULL) return;

    level = rt_hw_interrupt_disable();
    *out = g_ai_telemetry;
    out->running = g_uvc_ai_app.started;
    out->ai_ready = g_uvc_ai_app.ai_ready;
    out->mode = uvc_ai_get_mode();
    out->persist_target_ms = g_persist_target_ms;
    out->alert_active = uvc_ai_alert_visible(out->mode, rt_tick_get());
    rt_hw_interrupt_enable(level);
    out->danger_count = alert_get_danger_count();
}

uvc_ai_mode_t uvc_ai_app_get_mode(void)
{
    return uvc_ai_get_mode();
}

static int uvc_ai_app_init_model(void)
{
    uvc_ai_config_t config = {
        .initialized = 1U,
        .src_width = g_uvc_ai_app.src_width,
        .src_height = g_uvc_ai_app.src_height,
    };
    int ret = uvc_ai_init(&config);
    g_uvc_ai_app.ai_ready = ret == RT_EOK;
    g_uvc_ai_app.worker_running = g_uvc_ai_app.ai_ready;
    return ret;
}

int uvc_ai_app_set_mode(uvc_ai_mode_t mode)
{
    uvc_ai_mode_t target = (mode == UVC_AI_MODE_STEEL_BALL)
                           ? UVC_AI_MODE_STEEL_BALL
                           : UVC_AI_MODE_FATIGUE;

    int ret = RT_EOK;
    rt_mutex_take(&g_uvc_ai_lifecycle, RT_WAITING_FOREVER);
    if (uvc_ai_get_mode() == target) {
        if (g_uvc_ai_app.started && !g_uvc_ai_app.ai_ready)
            ret = uvc_ai_app_init_model();
        rt_mutex_release(&g_uvc_ai_lifecycle);
        return ret;
    }

    if (g_uvc_ai_app.started) {
        rt_tick_t start = rt_tick_get();

        g_uvc_ai_app.worker_running = RT_FALSE;
        while (g_uvc_ai_app.worker_busy) {
            if ((rt_tick_t)(rt_tick_get() - start) >= rt_tick_from_millisecond(500)) {
                g_uvc_ai_app.worker_running = g_uvc_ai_app.ai_ready;
                rt_mutex_release(&g_uvc_ai_lifecycle);
                return -RT_ETIMEOUT;
            }
            rt_thread_mdelay(5);
        }

        if (g_uvc_ai_app.ai_ready) {
            g_uvc_ai_app.ai_ready = RT_FALSE;
            uvc_ai_deinit();
        }
    }

    uvc_ai_app_reset_tracking();
    uvc_ai_set_mode(target);
    if (g_uvc_ai_app.started)
        ret = uvc_ai_app_init_model();
    rt_mutex_release(&g_uvc_ai_lifecycle);
    if (ret == RT_EOK)
        USB_LOG_INFO("AI mode switched to %s\r\n",
                     (target == UVC_AI_MODE_STEEL_BALL) ? "steel_ball" : "fatigue");
    else
        USB_LOG_ERR("AI mode switch init failed: %d\r\n", ret);
    return ret;
}

static void uvc_ai_app_overlay(const uvc_display_overlay_info_t *info)
{
    uvc_ai_result_t result_snapshot;
    const uvc_ai_result_t *result = &result_snapshot;
    uint8_t det_count;
    uint32_t i;
    char model_text[32];
    float   max_closed_conf = 0.0f;
    float   max_det_conf = 0.0f;
    float   thresh = alert_get_threshold();
    uint32_t persist_ms = 0U;
    rt_tick_t result_tick;
    uvc_ai_mode_t mode = uvc_ai_get_mode();

    uint32_t seq = uvc_ai_app_snapshot_result(&result_snapshot, &result_tick);
    rt_tick_t now = rt_tick_get();
    if (!result->valid ||
        (rt_tick_t)(now - result_tick) >= rt_tick_from_millisecond(UVC_AI_RESULT_MAX_AGE_MS)) {
        rt_base_t level = rt_hw_interrupt_disable();
        rt_memset(&g_ai_telemetry, 0, sizeof(g_ai_telemetry));
        rt_hw_interrupt_enable(level);
        g_closed_eye_tracking = RT_FALSE;
        g_alert_sent_this_cycle = RT_FALSE;
        return;
    }

    det_count = result->count;
    if (det_count > UVC_AI_MAX_PREDICTIONS) {
        det_count = UVC_AI_MAX_PREDICTIONS;
    }

    for (i = 0U; i < det_count; i++) {
        const int16_t *bbox = &result->bbox_int16[i * 4U];
        int16_t x0 = uvc_ai_app_map_x_to_lcd(info, bbox[0]);
        int16_t y0 = (mode == UVC_AI_MODE_STEEL_BALL)
                     ? uvc_ai_app_map_cam_y_to_lcd(info, bbox[1])
                     : uvc_ai_app_map_y_to_lcd(info, bbox[1]);
        int16_t x1 = uvc_ai_app_map_x_to_lcd(info, bbox[2]);
        int16_t y1 = (mode == UVC_AI_MODE_STEEL_BALL)
                     ? uvc_ai_app_map_cam_y_to_lcd(info, bbox[3])
                     : uvc_ai_app_map_y_to_lcd(info, bbox[3]);
        uint16_t color;
        char label_text[UVC_AI_LABEL_MAX_LEN];
        int16_t label_y;
        uint8_t is_closed_eye;

        if (x0 > x1) {
            int16_t tmp = x0;
            x0 = x1;
            x1 = tmp;
        }
        if (y0 > y1) {
            int16_t tmp = y0;
            y0 = y1;
            y1 = tmp;
        }

        if (result->conf[i] > max_det_conf) {
            max_det_conf = result->conf[i];
        }

        if (mode == UVC_AI_MODE_STEEL_BALL) {
            color = UVC_RGB565(16, 220, 120);
            (void)snprintf(label_text, sizeof(label_text), "BALL %.0f%%",
                           result->conf[i] * 100.0f);
        } else {
            /* Classify Closed Eye detections into 3 risk levels */
            is_closed_eye = (uint8_t)(strncmp(result->class_string[i], "Closed", 6U) == 0);

            if (is_closed_eye) {
                uint8_t risk_level = uvc_ai_get_risk_level(result->conf[i]);

                /* 跟踪最大闭眼置信度 */
                if (result->conf[i] > max_closed_conf)
                    max_closed_conf = result->conf[i];

                color = g_uvc_ai_risk_colors[risk_level];
                (void)snprintf(label_text, sizeof(label_text), "%s %s %.0f%%",
                               g_uvc_ai_risk_labels[risk_level],
                               result->class_string[i],
                               result->conf[i] * 100.0f);
            } else {
                if (result->class_id[i] < UVC_AI_NUM_CLASSES) {
                    color = g_uvc_ai_class_colors[result->class_id[i]];
                } else {
                    color = g_uvc_ai_unknown_color;
                }
                (void)snprintf(label_text, sizeof(label_text), "%s %.0f%%",
                               result->class_string[i], result->conf[i] * 100.0f);
            }
        }

        uvc_ai_app_draw_rect(info, x0, y0, x1, y1, color, UVC_AI_BOX_THICKNESS);

        label_y = (int16_t)(y0 - (int16_t)(UVC_AI_FONT_HEIGHT + UVC_AI_TEXT_PADDING_Y * 2U + 2U));
        if (label_y < (int16_t)info->dst_y_offset) {
            label_y = (int16_t)(y0 + 2);
        }
        uvc_ai_app_draw_text_box(info, x0, label_y, label_text, color, g_uvc_ai_text_color);
    }

    if (mode == UVC_AI_MODE_STEEL_BALL) {
        (void)snprintf(model_text, sizeof(model_text), "BALLS:%u %.1fms",
                       (unsigned)det_count, result->inference_ms);
    } else {
        (void)snprintf(model_text, sizeof(model_text), "Model %.1f ms", result->inference_ms);
    }
    uvc_ai_app_draw_text_box(info, 4, (int16_t)(info->dst_y_offset + 4U),
                             model_text, g_uvc_ai_model_bg, g_uvc_ai_text_color);

    /* Direct-FB 512x800 legacy mode keeps bottom T/H HUD overlay; LVGL stage shows cards separately */
    if (info->lcd_width > 360U) {
        char env_text[32];
        int16_t bx, by;
        int len;

        rt_snprintf(env_text, sizeof(env_text), "T:%.1fC  H:%.1f%%",
                    (double)g_aht_temp, (double)g_aht_humi);
        len = (int)rt_strlen(env_text);
        {
            uint16_t tw = (uint16_t)(len * 10 + (len - 1) * 2);
            bx = (int16_t)(info->lcd_width - tw - 4 - 180);
            by = (int16_t)(info->dst_y_offset + info->dst_height - 14 - 6);
            uvc_ai_app_fill_rect(info, (int16_t)(bx - 3), (int16_t)(by - 2),
                                 (int16_t)(bx + tw + 3), (int16_t)(by + 14 + 2),
                                 g_uvc_ai_model_bg);
            uvc_ai_app_draw_text_scaled(info, bx, by, env_text, g_uvc_ai_text_color, 2);
        }
    }

    /*
     * Persistence filter: closed eye must be continuously detected for g_persist_target_ms
     * before triggering an alert (Fatigue mode only).
     */
    if (mode == UVC_AI_MODE_FATIGUE && max_closed_conf >= thresh) {
        if (!g_closed_eye_tracking || seq != g_last_fatigue_seq) {
            if (!g_closed_eye_tracking ||
                (rt_tick_t)(result_tick - g_last_closed_eye_tick) >=
                rt_tick_from_millisecond(UVC_AI_RESULT_MAX_AGE_MS)) {
                g_closed_eye_tracking = RT_TRUE;
                g_closed_eye_start_tick = result_tick;
                g_alert_sent_this_cycle = RT_FALSE;
            }
            g_last_closed_eye_tick = result_tick;
            g_last_fatigue_seq = seq;
        }
        persist_ms = (uint32_t)(result_tick - g_closed_eye_start_tick) * (1000U / RT_TICK_PER_SECOND);
        if (persist_ms >= g_persist_target_ms && !g_alert_sent_this_cycle) {
            alert_danger(max_closed_conf, g_aht_temp, g_aht_humi);
            g_alert_flash_end = now + rt_tick_from_millisecond(2000);
            g_alert_sent_this_cycle = RT_TRUE;
        }
    } else {
        g_closed_eye_tracking = RT_FALSE;
        g_alert_sent_this_cycle = RT_FALSE;
    }

    {
        rt_base_t level = rt_hw_interrupt_disable();
        g_ai_telemetry.mode = mode;
        g_ai_telemetry.det_count = det_count;
        g_ai_telemetry.max_closed_conf = max_closed_conf;
        g_ai_telemetry.max_det_conf = max_det_conf;
        g_ai_telemetry.risk_level = (mode == UVC_AI_MODE_FATIGUE && max_closed_conf >= thresh)
                                    ? uvc_ai_get_risk_level(max_closed_conf)
                                    : UVC_AI_RISK_LOW;
        g_ai_telemetry.persist_ms = persist_ms;
        g_ai_telemetry.inference_ms = result->inference_ms;
        rt_hw_interrupt_enable(level);
    }

    /* Danger flash for 2s (Fatigue mode only) */
    if (uvc_ai_alert_visible(mode, now)) {
        int16_t banner_y = (int16_t)info->dst_y_offset;
        if (info->lcd_width <= 360U) {
            int16_t banner_h = 44;
            int16_t tx = (int16_t)((info->lcd_width - 12 * 12) / 2);
            int16_t ty = (int16_t)(banner_y + (banner_h - 14) / 2);
            uvc_ai_app_fill_rect(info, 0, banner_y,
                                 (int16_t)(info->lcd_width - 1),
                                 (int16_t)(banner_y + banner_h - 1),
                                 UVC_RGB565(180, 10, 10));
            uvc_ai_app_draw_text_scaled(info, tx, ty, "!! DANGER !!", UVC_RGB565(255, 255, 0), 2);
            uvc_ai_app_draw_rect(info, 0, banner_y,
                                 (int16_t)(info->lcd_width - 1),
                                 (int16_t)(info->dst_y_offset + info->dst_height - 1),
                                 UVC_RGB565(220, 30, 30), 3);
        } else {
            int16_t banner_h = (int16_t)(info->dst_height / 3);
            int16_t tx = (int16_t)((info->lcd_width - 12 * 30) / 2);
            int16_t ty = (int16_t)(banner_y + (banner_h - 35) / 2);
            uvc_ai_app_fill_rect(info, 0, banner_y,
                                 (int16_t)(info->lcd_width - 1),
                                 (int16_t)(banner_y + banner_h),
                                 UVC_RGB565(180, 10, 10));
            uvc_ai_app_draw_text_scaled(info, tx, ty, "!! DANGER !!", UVC_RGB565(255, 255, 0), 5);
            uvc_ai_app_draw_rect(info, 0, banner_y,
                                 (int16_t)(info->lcd_width - 1),
                                 (int16_t)(info->dst_y_offset + info->dst_height - 1),
                                 UVC_RGB565(180, 10, 10), 4);
        }
    }
}

static void uvc_ai_app_overlay_cb(const uvc_display_overlay_info_t *info, void *user_ctx)
{
    (void)user_ctx;
    if (info == RT_NULL || rt_mutex_take(&g_uvc_ai_lifecycle, 0) != RT_EOK)
        return;
    if (g_uvc_ai_app.started && g_uvc_ai_app.ai_ready && g_uvc_ai_app.worker_running)
        uvc_ai_app_overlay(info);
    rt_mutex_release(&g_uvc_ai_lifecycle);
}

void uvc_ai_app_enforce_mode(uint8_t *fmt, uint16_t *width, uint16_t *height)
{
    if ((fmt == RT_NULL) || (width == RT_NULL) || (height == RT_NULL)) {
        return;
    }

    if (*fmt != USBH_VIDEO_FORMAT_UNCOMPRESSED) {
        USB_LOG_WRN("AI mode requires YUYV, force fmt=0\r\n");
        *fmt = USBH_VIDEO_FORMAT_UNCOMPRESSED;
    }

    if ((*width != UVC_AI_CAMERA_WIDTH) || (*height != UVC_AI_CAMERA_HEIGHT)) {
        USB_LOG_WRN("AI mode requires 320x240, force %ux%u\r\n",
                    (unsigned)UVC_AI_CAMERA_WIDTH, (unsigned)UVC_AI_CAMERA_HEIGHT);
        *width = UVC_AI_CAMERA_WIDTH;
        *height = UVC_AI_CAMERA_HEIGHT;
    }
}

int uvc_ai_app_start(uint16_t src_width, uint16_t src_height)
{
    int ret;
    rt_mutex_take(&g_uvc_ai_lifecycle, RT_WAITING_FOREVER);
    if (g_uvc_ai_app.started) {
        rt_mutex_release(&g_uvc_ai_lifecycle);
        return -RT_EBUSY;
    }
    if (!g_uvc_ai_worker_initialized) {
        rt_sem_init(&g_uvc_ai_worker_wake, UVC_AI_WORKER_NAME, 0, RT_IPC_FLAG_FIFO);
        ret = rt_thread_init(&g_uvc_ai_worker_tcb, UVC_AI_WORKER_NAME,
                            uvc_ai_app_worker_thread_entry, RT_NULL,
                            g_uvc_ai_worker_stack, sizeof(g_uvc_ai_worker_stack),
                            UVC_AI_WORKER_PRIORITY, UVC_AI_WORKER_TICK);
        if (ret != RT_EOK) {
            rt_sem_detach(&g_uvc_ai_worker_wake);
            rt_mutex_release(&g_uvc_ai_lifecycle);
            return ret;
        }
        g_uvc_ai_worker_initialized = RT_TRUE;
        rt_thread_startup(&g_uvc_ai_worker_tcb);
    }
    memset(&g_uvc_ai_app, 0, sizeof(g_uvc_ai_app));
    g_uvc_ai_app.src_width = src_width ? src_width : UVC_AI_CAMERA_WIDTH;
    g_uvc_ai_app.src_height = src_height ? src_height : UVC_AI_CAMERA_HEIGHT;
    g_uvc_ai_app.started = RT_TRUE;

    uvc_ai_app_reset_tracking();
    uvc_display_set_overlay_callback(uvc_ai_app_overlay_cb, RT_NULL);
    ret = uvc_ai_app_init_model();
    rt_mutex_release(&g_uvc_ai_lifecycle);
    if (ret != RT_EOK) {
        USB_LOG_ERR("AI init failed, render-only mode\r\n");
        return RT_EOK;
    }

    USB_LOG_INFO("AI app started (async)\r\n");

    return RT_EOK;
}

void uvc_ai_app_process_frame(const struct usbh_videoframe *frame)
{
    if (frame == RT_NULL || frame->frame_buf == RT_NULL ||
        frame->frame_format != USBH_VIDEO_FORMAT_UNCOMPRESSED ||
        frame->frame_size < UVC_AI_FRAME_BYTES ||
        rt_mutex_take(&g_uvc_ai_lifecycle, 0) != RT_EOK) {
        return;
    }

    g_uvc_ai_app.frame_counter++;
    if (!g_uvc_ai_app.worker_running || g_uvc_ai_app.worker_busy ||
        (g_uvc_ai_app.frame_counter % UVC_AI_FRAMES_TO_SKIP) != 0U) {
        rt_mutex_release(&g_uvc_ai_lifecycle);
        return;
    }
    g_uvc_ai_app.worker_busy = RT_TRUE;
    rt_mutex_release(&g_uvc_ai_lifecycle);
    memcpy(g_uvc_ai_worker_frame, frame->frame_buf, UVC_AI_FRAME_BYTES);
    if (rt_sem_release(&g_uvc_ai_worker_wake) != RT_EOK)
        g_uvc_ai_app.worker_busy = RT_FALSE;
}

int uvc_ai_app_stop(void)
{
    rt_mutex_take(&g_uvc_ai_lifecycle, RT_WAITING_FOREVER);
    if (!g_uvc_ai_app.started) {
        rt_mutex_release(&g_uvc_ai_lifecycle);
        return RT_EOK;
    }

    uvc_display_set_overlay_callback(RT_NULL, RT_NULL);
    g_uvc_ai_app.worker_running = RT_FALSE;
    rt_tick_t start = rt_tick_get();
    while (g_uvc_ai_app.worker_busy) {
        if ((rt_tick_t)(rt_tick_get() - start) >= rt_tick_from_millisecond(250)) {
            rt_mutex_release(&g_uvc_ai_lifecycle);
            return -RT_ETIMEOUT;
        }
        rt_thread_mdelay(5);
    }

    if (g_uvc_ai_app.ai_ready) {
        uvc_ai_deinit();
        g_uvc_ai_app.ai_ready = RT_FALSE;
    }

    g_uvc_ai_app.started = RT_FALSE;
    uvc_ai_app_reset_tracking();
    rt_mutex_release(&g_uvc_ai_lifecycle);
    USB_LOG_INFO("AI app stopped\r\n");
    return RT_EOK;
}

static int cmd_ai_mode(int argc, char **argv)
{
    if (argc >= 2) {
        if (strcmp(argv[1], "steel_ball") == 0 || strcmp(argv[1], "ball") == 0) {
            int r = uvc_ai_app_set_mode(UVC_AI_MODE_STEEL_BALL);
            rt_kprintf("ai_mode=steel_ball ret=%d\n", r);
            return r;
        }
        if (strcmp(argv[1], "fatigue") == 0 || strcmp(argv[1], "eye") == 0) {
            int r = uvc_ai_app_set_mode(UVC_AI_MODE_FATIGUE);
            rt_kprintf("ai_mode=fatigue ret=%d\n", r);
            return r;
        }
    }
    rt_kprintf("ai_mode=%s running=%d ai_ready=%d thresh=%.2f\n",
               (uvc_ai_app_get_mode() == UVC_AI_MODE_STEEL_BALL) ? "steel_ball" : "fatigue",
               g_uvc_ai_app.started ? 1 : 0,
               g_uvc_ai_app.ai_ready ? 1 : 0,
               (double)uvc_ai_get_conf_threshold());
    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_ai_mode, ai_mode, Switch/query AI detection mode [fatigue|steel_ball|status]);

static void fill_synth_steel_ball_yuyv(uint8_t *yuyv)
{
    uint32_t y;
    uint32_t x;
    const float cx = 199.0f;
    const float cy = 180.0f;
    const float r = 17.0f;
    const float r2 = r * r;

    for (y = 0U; y < UVC_AI_CAMERA_HEIGHT; y++) {
        uint8_t *row = yuyv + y * (UVC_AI_CAMERA_WIDTH * 2U);
        for (x = 0U; x < UVC_AI_CAMERA_WIDTH; x += 2U) {
            uint32_t px;
            uint8_t luma[2];
            for (px = 0U; px < 2U; px++) {
                float fx = (float)(x + px);
                float fy = (float)y;
                float dx = fx - cx;
                float dy = fy - cy;
                float d2 = dx * dx + dy * dy;
                int32_t val;

                if (d2 <= r2) {
                    float nx = dx / r;
                    float ny = dy / r;
                    float nz = sqrtf(fmaxf(0.0f, 1.0f - nx * nx - ny * ny));
                    float diff = fmaxf(0.0f, nx * (-0.40f) + ny * (-0.50f) + nz * 0.76f);
                    float spec_base = fmaxf(0.0f, nx * (-0.25f) + ny * (-0.32f) + nz * 0.91f);
                    float spec = powf(spec_base, 24.0f);
                    val = (int32_t)(45.0f + 150.0f * diff + 220.0f * spec);
                } else {
                    val = 140 + (((int32_t)(x + px) * 40) / 320) - 20;
                }
                if (val < 0) val = 0;
                if (val > 255) val = 255;
                luma[px] = (uint8_t)val;
            }
            row[x * 2U + 0U] = luma[0];
            row[x * 2U + 1U] = 128U;
            row[x * 2U + 2U] = luma[1];
            row[x * 2U + 3U] = 128U;
        }
    }
}

typedef struct {
    struct rt_semaphore done;
    int ret;
} ai_selftest_ctx_t;

static void ai_selftest_thread_entry(void *param)
{
    ai_selftest_ctx_t *ctx = (ai_selftest_ctx_t *)param;
    uvc_ai_config_t cfg;
    uvc_ai_result_t res;
    int ret;
    uint8_t i;

    cfg.initialized = 1U;
    cfg.src_width = UVC_AI_CAMERA_WIDTH;
    cfg.src_height = UVC_AI_CAMERA_HEIGHT;

    ret = uvc_ai_init(&cfg);
    if (ret != RT_EOK) {
        rt_kprintf("ai_selftest: init failed (%d)\n", ret);
        ctx->ret = ret;
        rt_sem_release(&ctx->done);
        return;
    }

    fill_synth_steel_ball_yuyv(g_uvc_ai_worker_frame);
    ret = uvc_ai_process_yuyv(g_uvc_ai_worker_frame, UVC_AI_FRAME_BYTES, &res);
    if (ret == RT_EOK && uvc_ai_get_mode() == UVC_AI_MODE_STEEL_BALL) {
        uvc_ai_result_t sample;
        for (unsigned int trial = 0; trial < 4U; trial++) {
            memset(g_uvc_ai_worker_frame, 128, UVC_AI_FRAME_BYTES);
            ret = uvc_ai_process_yuyv(g_uvc_ai_worker_frame, UVC_AI_FRAME_BYTES, &sample);
            if (ret != RT_EOK || sample.count != 0U) {
                rt_kprintf("ai_selftest: blank frame check failed (trial=%u)\n", trial + 1U);
                ret = -RT_ERROR;
                break;
            }
            fill_synth_steel_ball_yuyv(g_uvc_ai_worker_frame);
            ret = uvc_ai_process_yuyv(g_uvc_ai_worker_frame, UVC_AI_FRAME_BYTES, &sample);
            if (ret != RT_EOK || sample.count != res.count ||
                memcmp(res.bbox_int16, sample.bbox_int16, sizeof(res.bbox_int16)) != 0 ||
                memcmp(res.conf, sample.conf, sizeof(res.conf)) != 0) {
                rt_kprintf("ai_selftest: repeated frame check failed (trial=%u)\n", trial + 1U);
                ret = -RT_ERROR;
                break;
            }
        }
        if (ret == RT_EOK)
            rt_kprintf("ai_selftest: 4 alternating blank/target frames passed\n");
    }
    uvc_ai_deinit();

    if (ret != RT_EOK) {
        rt_kprintf("ai_selftest: infer failed (%d)\n", ret);
        ctx->ret = ret;
        rt_sem_release(&ctx->done);
        return;
    }

    rt_kprintf("ai_selftest: mode=%s inf=%.1fms det=%u\n",
               (uvc_ai_get_mode() == UVC_AI_MODE_STEEL_BALL) ? "steel_ball" : "fatigue",
               (double)res.inference_ms, (unsigned)res.count);
    for (i = 0U; i < res.count; i++) {
        rt_kprintf("  #%u %s %.1f%% [%d,%d,%d,%d]\n",
                   (unsigned)i, res.class_string[i], (double)(res.conf[i] * 100.0f),
                   res.bbox_int16[i * 4U + 0U], res.bbox_int16[i * 4U + 1U],
                   res.bbox_int16[i * 4U + 2U], res.bbox_int16[i * 4U + 3U]);
    }
    ctx->ret = RT_EOK;
    rt_sem_release(&ctx->done);
}

static int cmd_ai_selftest(int argc, char **argv)
{
    ai_selftest_ctx_t ctx;
    rt_thread_t tid;

    (void)argc;
    (void)argv;

    rt_mutex_take(&g_uvc_ai_lifecycle, RT_WAITING_FOREVER);
    if (g_uvc_ai_app.started) {
        rt_mutex_release(&g_uvc_ai_lifecycle);
        rt_kprintf("ai_selftest: busy (camera running)\n");
        return -RT_EBUSY;
    }

    rt_sem_init(&ctx.done, "aiselft", 0, RT_IPC_FLAG_FIFO);
    ctx.ret = -RT_ERROR;

    tid = rt_thread_create("ai_test", ai_selftest_thread_entry, &ctx,
                           16384, UVC_AI_WORKER_PRIORITY, UVC_AI_WORKER_TICK);
    if (tid == RT_NULL) {
        rt_sem_detach(&ctx.done);
        rt_mutex_release(&g_uvc_ai_lifecycle);
        rt_kprintf("ai_selftest: thread create failed\n");
        return -RT_ENOMEM;
    }
    rt_thread_startup(tid);
    rt_sem_take(&ctx.done, RT_WAITING_FOREVER);
    rt_sem_detach(&ctx.done);
    rt_mutex_release(&g_uvc_ai_lifecycle);
    return ctx.ret;
}
MSH_CMD_EXPORT_ALIAS(cmd_ai_selftest, ai_selftest, Run one-shot AI NPU self-test in active mode);
