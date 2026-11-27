#include <rtthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#include "uvc_ai_app.h"
#include "uvc_ai.h"
#include "sb_tracker.h"
#include "sd_assets.h"
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
/* Keep AI worker below LVGL(15), uvc_frm(16), and uvc_ctl(17) so CPU postproc never starves frame drain or USB recovery. */
#define UVC_AI_WORKER_PRIORITY     18U
#define UVC_AI_WORKER_TICK         5U
#define UVC_AI_WORKER_NAME         "uvc_aiwk"
#define UVC_AI_WORK_AVAILABLE      1U
#define UVC_AI_RESULT_MAX_AGE_MS   500
#define UVC_AI_FPS_RING_CAP        48U
#define UVC_AI_FPS_WINDOW_MS       2000U
#define UVC_AI_FPS_STALE_MS        500U
#define UVC_AI_RECOVERY_ERRORS     2U

typedef struct {
    char ch;
    uint8_t rows[UVC_AI_FONT_HEIGHT];
} uvc_font5x7_t;

typedef struct {
    rt_bool_t started;
    rt_bool_t ai_ready;
    rt_bool_t ai_failed;
    volatile rt_bool_t worker_busy;
    volatile rt_bool_t frame_copying;
    rt_bool_t frame_pending;
    rt_bool_t tracker_reset_req;
    rt_bool_t reload_req;
    rt_bool_t recover_req;
    uint8_t worker_slot;
    uint8_t consecutive_errors;
    uint32_t recoveries;
    rt_tick_t frame_tick;
    uvc_ai_mode_t desired_mode;
    uvc_ai_mode_t active_mode;
    uint32_t session_gen;
    uint32_t frame_counter;
    uint32_t infer_count;
    uint32_t infer_errors;
    uint32_t mode_switches;
    uint32_t dropped_busy_frames;
    uint16_t src_width;
    uint16_t src_height;
    uvc_ai_result_t latest_result;
    rt_tick_t result_tick;
    rt_tick_t result_input_tick;
    uint32_t result_seq;
    rt_tick_t last_retry_tick;
    rt_tick_t fps_ticks[UVC_AI_FPS_RING_CAP];
    uint8_t fps_head;
    uint8_t fps_count;
    rt_tick_t last_infer_done_tick;
} uvc_ai_app_ctx_t;

static uvc_ai_app_ctx_t g_uvc_ai_app;
static struct rt_mutex g_uvc_ai_lifecycle;
/* Place 64KB AI worker stack in NOLOAD D-Cacheable SoCMEM (frees 64KB DTCM + 64KB Flash). */
static UVC_AI_STACK_SECTION rt_uint8_t g_uvc_ai_worker_stack[UVC_AI_WORKER_STACK_SIZE];
static struct rt_thread g_uvc_ai_worker_tcb;
static struct rt_event g_uvc_ai_worker_wake;
static rt_bool_t g_uvc_ai_worker_initialized;
static volatile rt_bool_t g_hw_ai_ready;
static __attribute__((section(".cy_ml_arena_fast_data"), aligned(32)))
    uint8_t g_uvc_ai_worker_frames[2][UVC_AI_FRAME_BYTES];
static sb_tracker_t g_sb_tracker;

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
    g_uvc_ai_app.desired_mode = uvc_ai_get_mode();
    g_uvc_ai_app.active_mode = g_uvc_ai_app.desired_mode;
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

static void uvc_ai_app_publish_result_locked(const uvc_ai_result_t *result, rt_tick_t input_tick)
{
    memcpy(&g_uvc_ai_app.latest_result, result, sizeof(g_uvc_ai_app.latest_result));
    g_uvc_ai_app.result_tick = rt_tick_get();
    g_uvc_ai_app.result_input_tick = input_tick;
    g_uvc_ai_app.result_seq++;
}

static uint32_t uvc_ai_app_snapshot_result(uvc_ai_result_t *result, rt_tick_t *tick,
                                           rt_tick_t *input_tick, uvc_ai_mode_t *mode)
{
    rt_base_t level = rt_hw_interrupt_disable();
    memcpy(result, &g_uvc_ai_app.latest_result, sizeof(*result));
    *tick = g_uvc_ai_app.result_tick;
    *input_tick = g_uvc_ai_app.result_input_tick;
    *mode = g_uvc_ai_app.desired_mode;
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

static void uvc_ai_fps_reset_locked(void)
{
    g_uvc_ai_app.fps_head = 0U;
    g_uvc_ai_app.fps_count = 0U;
    g_uvc_ai_app.last_infer_done_tick = 0U;
    rt_memset(g_uvc_ai_app.fps_ticks, 0, sizeof(g_uvc_ai_app.fps_ticks));
}

static void uvc_ai_fps_record_locked(rt_tick_t now)
{
    g_uvc_ai_app.fps_ticks[g_uvc_ai_app.fps_head] = now;
    g_uvc_ai_app.fps_head = (uint8_t)((g_uvc_ai_app.fps_head + 1U) % UVC_AI_FPS_RING_CAP);
    if (g_uvc_ai_app.fps_count < UVC_AI_FPS_RING_CAP) {
        g_uvc_ai_app.fps_count++;
    }
    g_uvc_ai_app.last_infer_done_tick = now;
}

static float uvc_ai_fps_compute_locked(rt_tick_t now)
{
    rt_tick_t window_ticks;
    rt_tick_t stale_ticks;
    rt_tick_t oldest = 0U;
    rt_tick_t newest = 0U;
    uint8_t valid_n = 0U;
    uint8_t start_idx;
    uint8_t i;

    if (!g_uvc_ai_app.started || !g_uvc_ai_app.ai_ready || g_uvc_ai_app.reload_req ||
        (g_uvc_ai_app.active_mode != g_uvc_ai_app.desired_mode) ||
        (g_uvc_ai_app.fps_count < 2U)) {
        return 0.0f;
    }

    stale_ticks = rt_tick_from_millisecond(UVC_AI_FPS_STALE_MS);
    if ((rt_tick_t)(now - g_uvc_ai_app.last_infer_done_tick) > stale_ticks) {
        return 0.0f;
    }

    window_ticks = rt_tick_from_millisecond(UVC_AI_FPS_WINDOW_MS);
    start_idx = (uint8_t)((g_uvc_ai_app.fps_head + UVC_AI_FPS_RING_CAP - g_uvc_ai_app.fps_count) %
                          UVC_AI_FPS_RING_CAP);

    for (i = 0U; i < g_uvc_ai_app.fps_count; i++) {
        rt_tick_t ts = g_uvc_ai_app.fps_ticks[(start_idx + i) % UVC_AI_FPS_RING_CAP];
        if ((rt_tick_t)(now - ts) <= window_ticks) {
            if (valid_n == 0U) {
                oldest = ts;
            }
            newest = ts;
            valid_n++;
        }
    }

    if ((valid_n >= 2U) && ((rt_tick_t)(newest - oldest) > 0U)) {
        return (float)(valid_n - 1U) * (float)RT_TICK_PER_SECOND /
               (float)(rt_tick_t)(newest - oldest);
    }

    return 0.0f;
}

static void uvc_ai_app_reset_tracking_locked(void);

static int uvc_ai_app_init_model_locked(uvc_ai_mode_t mode, uint16_t src_w, uint16_t src_h)
{
    uvc_ai_config_t config = {
        .initialized = 1U,
        .src_width = src_w ? src_w : UVC_AI_CAMERA_WIDTH,
        .src_height = src_h ? src_h : UVC_AI_CAMERA_HEIGHT,
    };
    uvc_ai_set_mode(mode);
    return uvc_ai_init(&config);
}

static void uvc_ai_app_worker_thread_entry(void *parameter)
{
    uvc_ai_result_t result;
    uvc_ai_result_t low_result;
    uvc_ai_result_t tracked;

    (void)parameter;

    while (1) {
        if (rt_event_recv(&g_uvc_ai_worker_wake, UVC_AI_WORK_AVAILABLE,
                          RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR, RT_WAITING_FOREVER, RT_NULL) != RT_EOK) {
            continue;
        }

        for (;;) {
            rt_base_t level;
            rt_bool_t started;
            rt_bool_t reload_req;
            rt_bool_t recover_req;
            rt_bool_t has_frame;
            rt_bool_t failed;
            uvc_ai_mode_t desired_mode;
            uvc_ai_mode_t active_mode;
            uint16_t src_w;
            uint16_t src_h;
            uint32_t gen;
            rt_tick_t last_retry;

            level = rt_hw_interrupt_disable();
            started = g_uvc_ai_app.started;
            reload_req = g_uvc_ai_app.reload_req;
            recover_req = g_uvc_ai_app.recover_req;
            has_frame = g_uvc_ai_app.frame_pending;
            failed = g_uvc_ai_app.ai_failed;
            desired_mode = g_uvc_ai_app.desired_mode;
            active_mode = g_uvc_ai_app.active_mode;
            src_w = g_uvc_ai_app.src_width;
            src_h = g_uvc_ai_app.src_height;
            gen = g_uvc_ai_app.session_gen;
            last_retry = g_uvc_ai_app.last_retry_tick;

            if (!started) {
                g_uvc_ai_app.frame_pending = RT_FALSE;
                g_uvc_ai_app.ai_ready = RT_FALSE;
                if (!g_hw_ai_ready) {
                    g_uvc_ai_app.worker_busy = RT_FALSE;
                    rt_hw_interrupt_enable(level);
                    break;
                }
                g_uvc_ai_app.worker_busy = RT_TRUE;
                rt_hw_interrupt_enable(level);

                rt_mutex_take(&g_uvc_ai_lifecycle, RT_WAITING_FOREVER);
                if (g_hw_ai_ready) {
                    uvc_ai_deinit();
                    g_hw_ai_ready = RT_FALSE;
                }
                sb_tracker_reset(&g_sb_tracker);
                rt_mutex_release(&g_uvc_ai_lifecycle);

                level = rt_hw_interrupt_disable();
                g_uvc_ai_app.ai_ready = RT_FALSE;
                g_uvc_ai_app.worker_busy = RT_FALSE;
                rt_hw_interrupt_enable(level);
                continue;
            }

            if (reload_req || recover_req || !g_hw_ai_ready || (active_mode != desired_mode)) {
                rt_bool_t retry_blocked = RT_FALSE;
                if (!reload_req && !recover_req && failed && (active_mode == desired_mode) && !g_hw_ai_ready) {
                    rt_tick_t now = rt_tick_get();
                    if ((desired_mode != UVC_AI_MODE_STEEL_BALL) ||
                        !sdcard_is_mounted() ||
                        ((rt_tick_t)(now - last_retry) < rt_tick_from_millisecond(500))) {
                        retry_blocked = RT_TRUE;
                    }
                }
                if (retry_blocked) {
                    g_uvc_ai_app.frame_pending = RT_FALSE;
                    g_uvc_ai_app.worker_busy = RT_FALSE;
                    rt_hw_interrupt_enable(level);
                    break;
                }

                g_uvc_ai_app.worker_busy = RT_TRUE;
                g_uvc_ai_app.reload_req = RT_FALSE;
                g_uvc_ai_app.recover_req = RT_FALSE;
                g_uvc_ai_app.frame_pending = RT_FALSE;
                g_uvc_ai_app.ai_ready = RT_FALSE;
                rt_hw_interrupt_enable(level);

                rt_mutex_take(&g_uvc_ai_lifecycle, RT_WAITING_FOREVER);
                if (g_hw_ai_ready) {
                    uvc_ai_deinit();
                    g_hw_ai_ready = RT_FALSE;
                }
                if (reload_req) {
                    uvc_ai_invalidate_steel_ball_cache();
                }

                level = rt_hw_interrupt_disable();
                rt_bool_t still_needed = g_uvc_ai_app.started &&
                                         (g_uvc_ai_app.session_gen == gen) &&
                                         (g_uvc_ai_app.desired_mode == desired_mode) &&
                                         !g_uvc_ai_app.reload_req;
                rt_hw_interrupt_enable(level);

                int init_ret = RT_EOK;
                if (still_needed) {
                    init_ret = uvc_ai_app_init_model_locked(desired_mode, src_w, src_h);
                    if (init_ret == RT_EOK) {
                        sb_tracker_reset(&g_sb_tracker);
                    }
                }
                rt_mutex_release(&g_uvc_ai_lifecycle);

                level = rt_hw_interrupt_disable();
                if (still_needed) {
                    g_hw_ai_ready = (init_ret == RT_EOK);
                    g_uvc_ai_app.active_mode = desired_mode;
                    if (g_uvc_ai_app.session_gen == gen) {
                        g_uvc_ai_app.ai_ready = g_hw_ai_ready;
                        g_uvc_ai_app.ai_failed = !g_hw_ai_ready;
                        g_uvc_ai_app.consecutive_errors = 0U;
                        g_uvc_ai_app.last_retry_tick = rt_tick_get();
                    }
                }
                g_uvc_ai_app.worker_busy = RT_FALSE;
                rt_hw_interrupt_enable(level);
                continue;
            }

            /* A rapid stop/start or mode round-trip may keep the right model loaded. */
            g_uvc_ai_app.ai_ready = RT_TRUE;
            if (!has_frame || g_uvc_ai_app.frame_copying) {
                g_uvc_ai_app.worker_busy = RT_FALSE;
                rt_hw_interrupt_enable(level);
                break;
            }

            g_uvc_ai_app.frame_pending = RT_FALSE;
            g_uvc_ai_app.worker_busy = RT_TRUE;
            g_uvc_ai_app.worker_slot ^= 1U;
            const uint8_t *frame = g_uvc_ai_worker_frames[g_uvc_ai_app.worker_slot];
            rt_tick_t input_tick = g_uvc_ai_app.frame_tick;
            rt_bool_t reset_tracker = g_uvc_ai_app.tracker_reset_req;
            g_uvc_ai_app.tracker_reset_req = RT_FALSE;
            rt_hw_interrupt_enable(level);

            int ret;
            const uvc_ai_result_t *output = &result;
            rt_mutex_take(&g_uvc_ai_lifecycle, RT_WAITING_FOREVER);
            if (active_mode == UVC_AI_MODE_STEEL_BALL) {
                ret = uvc_ai_process_yuyv_ex(frame,
                                             UVC_AI_FRAME_BYTES,
                                             &result,
                                             &low_result);
                if (ret == RT_EOK) {
                    uint32_t frame_ms = (uint32_t)(((uint64_t)input_tick * 1000ULL) /
                                                 (uint64_t)RT_TICK_PER_SECOND);
                    if (reset_tracker) {
                        sb_tracker_reset(&g_sb_tracker);
                    }
                    sb_tracker_update(&g_sb_tracker, &result, &low_result, frame_ms, &tracked);
                    output = &tracked;
                }
            } else {
                ret = uvc_ai_process_yuyv(frame,
                                          UVC_AI_FRAME_BYTES,
                                          &result);
            }
            rt_mutex_release(&g_uvc_ai_lifecycle);
            if (ret != RT_EOK) {
                uvc_ai_reset_result(&result);
            }

            rt_bool_t recover = RT_FALSE;
            level = rt_hw_interrupt_disable();
            g_uvc_ai_app.infer_count++;
            if (ret != RT_EOK) {
                g_uvc_ai_app.infer_errors++;
            }
            if (g_uvc_ai_app.started && g_uvc_ai_app.session_gen == gen) {
                if (ret == RT_EOK) {
                    g_uvc_ai_app.consecutive_errors = 0U;
                    uvc_ai_fps_record_locked(rt_tick_get());
                } else {
                    g_uvc_ai_app.tracker_reset_req |= reset_tracker;
                    if (++g_uvc_ai_app.consecutive_errors >= UVC_AI_RECOVERY_ERRORS) {
                        g_uvc_ai_app.recover_req = RT_TRUE;
                        g_uvc_ai_app.ai_ready = RT_FALSE;
                        g_uvc_ai_app.tracker_reset_req = RT_TRUE;
                        g_uvc_ai_app.recoveries++;
                        uvc_ai_fps_reset_locked();
                        recover = RT_TRUE;
                    }
                }
                uvc_ai_app_publish_result_locked(output, input_tick);
            }
            g_uvc_ai_app.worker_busy = RT_FALSE;
            rt_hw_interrupt_enable(level);

            if (recover) {
                USB_LOG_WRN("AI model restarting after inference errors (%d)\r\n", ret);
            }
        }
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

static void uvc_ai_app_reset_tracking_locked(void)
{
    g_alert_flash_end = 0;
    g_closed_eye_tracking = RT_FALSE;
    g_alert_sent_this_cycle = RT_FALSE;
    rt_memset(&g_ai_telemetry, 0, sizeof(g_ai_telemetry));
    uvc_ai_reset_result(&g_uvc_ai_app.latest_result);
    g_uvc_ai_app.result_tick = 0;
    g_uvc_ai_app.result_input_tick = 0;
    g_uvc_ai_app.result_seq++;
    /* Defer sb_tracker_reset() to uvc_aiwk under g_uvc_ai_lifecycle to prevent concurrent mutation. */
    g_uvc_ai_app.tracker_reset_req = RT_TRUE;
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
    rt_tick_t now;
    if (out == RT_NULL) return;

    now = rt_tick_get();
    level = rt_hw_interrupt_disable();
    *out = g_ai_telemetry;
    out->running = g_uvc_ai_app.started;
    out->ai_ready = g_uvc_ai_app.ai_ready;
    out->mode = g_uvc_ai_app.desired_mode;
    out->persist_target_ms = g_persist_target_ms;
    out->ai_fps = uvc_ai_fps_compute_locked(now);
    out->alert_active = uvc_ai_alert_visible(out->mode, now);
    rt_hw_interrupt_enable(level);
    out->danger_count = alert_get_danger_count();
}

rt_bool_t uvc_ai_app_is_failed(void)
{
    return g_uvc_ai_app.ai_failed;
}

uvc_ai_mode_t uvc_ai_app_get_mode(void)
{
    return g_uvc_ai_app.desired_mode;
}

int uvc_ai_app_set_mode(uvc_ai_mode_t mode)
{
    uvc_ai_mode_t target = (mode == UVC_AI_MODE_STEEL_BALL)
                           ? UVC_AI_MODE_STEEL_BALL
                           : UVC_AI_MODE_FATIGUE;
    rt_base_t level;
    rt_bool_t changed = RT_FALSE;
    rt_bool_t started;

    level = rt_hw_interrupt_disable();
    if (g_uvc_ai_app.desired_mode != target) {
        g_uvc_ai_app.desired_mode = target;
        g_uvc_ai_app.ai_ready = RT_FALSE;
        g_uvc_ai_app.ai_failed = RT_FALSE;
        g_uvc_ai_app.frame_pending = RT_FALSE;
        g_uvc_ai_app.session_gen++;
        g_uvc_ai_app.mode_switches++;
        g_uvc_ai_app.consecutive_errors = 0U;
        uvc_ai_fps_reset_locked();
        uvc_ai_app_reset_tracking_locked();
        changed = RT_TRUE;
    }
    started = g_uvc_ai_app.started;
    rt_hw_interrupt_enable(level);

    if (changed) {
        USB_LOG_INFO("AI mode requested: %s\r\n",
                     (target == UVC_AI_MODE_STEEL_BALL) ? "steel_ball" : "fatigue");
    }

    if (started && g_uvc_ai_worker_initialized) {
        rt_event_send(&g_uvc_ai_worker_wake, UVC_AI_WORK_AVAILABLE);
    }
    return RT_EOK;
}

static uint8_t uvc_ai_app_draw_detections(const uvc_display_overlay_info_t *info,
                                          const uvc_ai_result_t *result,
                                          uvc_ai_mode_t mode,
                                          float ai_fps,
                                          uint32_t age_ms,
                                          float *max_det_conf_out,
                                          float *max_closed_conf_out)
{
    uint8_t det_count;
    uint32_t i;
    char model_text[40];
    float max_closed_conf = 0.0f;
    float max_det_conf = 0.0f;
    float inf_ms;
    float fps_disp;

    if ((info == RT_NULL) || (result == RT_NULL)) {
        return 0U;
    }

    det_count = result->count;
    if (det_count > UVC_AI_MAX_PREDICTIONS) {
        det_count = UVC_AI_MAX_PREDICTIONS;
    }

    for (i = 0U; i < det_count; i++) {
        const int16_t *bbox = &result->bbox_int16[i * 4U];
        int16_t projected_box[4];
        if (mode == UVC_AI_MODE_STEEL_BALL && det_count == 1U && age_ms != 0U) {
            sb_tracker_project_single(result, age_ms, projected_box);
            bbox = projected_box;
        }
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
        float conf = isfinite(result->conf[i]) ? result->conf[i] : 0.0f;

        if (conf < 0.0f) {
            conf = 0.0f;
        } else if (conf > 1.0f) {
            conf = 1.0f;
        }

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

        if (conf > max_det_conf) {
            max_det_conf = conf;
        }

        if (mode == UVC_AI_MODE_STEEL_BALL) {
            color = UVC_RGB565(16, 220, 120);
            (void)rt_snprintf(label_text, sizeof(label_text), "BALL #%u %u%%",
                              (unsigned)result->track_id[i], (unsigned)(conf * 100.0f + 0.5f));
        } else {
            /* Classify Closed Eye detections into 3 risk levels */
            is_closed_eye = (uint8_t)(strncmp(result->class_string[i], "Closed", 6U) == 0);

            if (is_closed_eye) {
                uint8_t risk_level = uvc_ai_get_risk_level(conf);

                /* 跟踪最大闭眼置信度 */
                if (conf > max_closed_conf)
                    max_closed_conf = conf;

                color = g_uvc_ai_risk_colors[risk_level];
                (void)rt_snprintf(label_text, sizeof(label_text), "%s %s %.0f%%",
                                  g_uvc_ai_risk_labels[risk_level],
                                  result->class_string[i],
                                  (double)(conf * 100.0f));
            } else {
                if (result->class_id[i] < UVC_AI_NUM_CLASSES) {
                    color = g_uvc_ai_class_colors[result->class_id[i]];
                } else {
                    color = g_uvc_ai_unknown_color;
                }
                (void)rt_snprintf(label_text, sizeof(label_text), "%s %.0f%%",
                                  result->class_string[i], (double)(conf * 100.0f));
            }
        }

        uvc_ai_app_draw_rect(info, x0, y0, x1, y1, color, UVC_AI_BOX_THICKNESS);

        label_y = (int16_t)(y0 - (int16_t)(UVC_AI_FONT_HEIGHT + UVC_AI_TEXT_PADDING_Y * 2U + 2U));
        if (label_y < (int16_t)info->dst_y_offset) {
            label_y = (int16_t)(y0 + 2);
        }
        uvc_ai_app_draw_text_box(info, x0, label_y, label_text, color, g_uvc_ai_text_color);
    }

    inf_ms = isfinite(result->inference_ms) ? result->inference_ms : 0.0f;
    fps_disp = isfinite(ai_fps) ? ai_fps : 0.0f;
    if (mode == UVC_AI_MODE_STEEL_BALL) {
        (void)rt_snprintf(model_text, sizeof(model_text), "AI %.1fFPS BALL:%u %.0fMS",
                          (double)fps_disp, (unsigned)det_count, (double)inf_ms);
    } else {
        (void)rt_snprintf(model_text, sizeof(model_text), "AI %.1fFPS %.0fMS",
                          (double)fps_disp, (double)inf_ms);
    }
    uvc_ai_app_draw_text_box(info, 4, (int16_t)(info->dst_y_offset + 4U),
                             model_text, g_uvc_ai_model_bg, g_uvc_ai_text_color);

    if (max_det_conf_out != RT_NULL) {
        *max_det_conf_out = max_det_conf;
    }
    if (max_closed_conf_out != RT_NULL) {
        *max_closed_conf_out = max_closed_conf;
    }
    return det_count;
}

static void uvc_ai_app_overlay(const uvc_display_overlay_info_t *info)
{
    uvc_ai_result_t result_snapshot;
    const uvc_ai_result_t *result = &result_snapshot;
    uint8_t det_count;
    float   max_closed_conf = 0.0f;
    float   max_det_conf = 0.0f;
    float   thresh = alert_get_threshold();
    uint32_t persist_ms = 0U;
    rt_tick_t result_tick;
    rt_tick_t input_tick;
    uvc_ai_mode_t mode;
    float inf_ms;
    float ai_fps;

    uint32_t seq = uvc_ai_app_snapshot_result(&result_snapshot, &result_tick, &input_tick, &mode);
    rt_tick_t now = rt_tick_get();
    rt_tick_t input_age = now - input_tick;
    {
        rt_base_t level = rt_hw_interrupt_disable();
        ai_fps = uvc_ai_fps_compute_locked(now);
        rt_hw_interrupt_enable(level);
    }
    if (!result->valid ||
        (rt_tick_t)(now - result_tick) >= rt_tick_from_millisecond(UVC_AI_RESULT_MAX_AGE_MS) ||
        (mode == UVC_AI_MODE_STEEL_BALL &&
         input_age >= rt_tick_from_millisecond(UVC_AI_RESULT_MAX_AGE_MS))) {
        rt_base_t level = rt_hw_interrupt_disable();
        rt_memset(&g_ai_telemetry, 0, sizeof(g_ai_telemetry));
        rt_hw_interrupt_enable(level);
        g_closed_eye_tracking = RT_FALSE;
        g_alert_sent_this_cycle = RT_FALSE;
        return;
    }

    uint32_t age_ms = (uint32_t)input_age * 1000U / RT_TICK_PER_SECOND;
    det_count = uvc_ai_app_draw_detections(info, result, mode, ai_fps, age_ms,
                                           &max_det_conf, &max_closed_conf);
    inf_ms = isfinite(result->inference_ms) ? result->inference_ms : 0.0f;

    /* Direct-FB 512x800 legacy mode keeps bottom T/H HUD overlay; LVGL stage shows cards separately */
    if (info->lcd_width > 360U) {
        char env_text[32];
        int16_t bx, by;
        int len;
        float t_disp = isfinite(g_aht_temp) ? g_aht_temp : 0.0f;
        float h_disp = isfinite(g_aht_humi) ? g_aht_humi : 0.0f;

        rt_snprintf(env_text, sizeof(env_text), "T:%.1fC  H:%.1f%%",
                    (double)t_disp, (double)h_disp);
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
        g_ai_telemetry.inference_ms = inf_ms;
        g_ai_telemetry.ai_fps = isfinite(ai_fps) ? ai_fps : 0.0f;
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
    rt_base_t level;
    rt_bool_t ready;
    (void)user_ctx;
    if (info == RT_NULL)
        return;
    level = rt_hw_interrupt_disable();
    ready = g_uvc_ai_app.started && g_uvc_ai_app.ai_ready &&
            (g_uvc_ai_app.active_mode == g_uvc_ai_app.desired_mode) &&
            !g_uvc_ai_app.reload_req;
    rt_hw_interrupt_enable(level);
    if (ready)
        uvc_ai_app_overlay(info);
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
    rt_base_t level;

    if (!g_uvc_ai_worker_initialized) {
        rt_event_init(&g_uvc_ai_worker_wake, UVC_AI_WORKER_NAME, RT_IPC_FLAG_FIFO);
        ret = rt_thread_init(&g_uvc_ai_worker_tcb, UVC_AI_WORKER_NAME,
                             uvc_ai_app_worker_thread_entry, RT_NULL,
                             g_uvc_ai_worker_stack, sizeof(g_uvc_ai_worker_stack),
                             UVC_AI_WORKER_PRIORITY, UVC_AI_WORKER_TICK);
        if (ret != RT_EOK) {
            rt_event_detach(&g_uvc_ai_worker_wake);
            return ret;
        }
        g_uvc_ai_worker_initialized = RT_TRUE;
        rt_thread_startup(&g_uvc_ai_worker_tcb);
    }

    level = rt_hw_interrupt_disable();
    if (g_uvc_ai_app.started || g_uvc_ai_app.frame_copying) {
        rt_hw_interrupt_enable(level);
        return -RT_EBUSY;
    }
    g_uvc_ai_app.src_width = src_width ? src_width : UVC_AI_CAMERA_WIDTH;
    g_uvc_ai_app.src_height = src_height ? src_height : UVC_AI_CAMERA_HEIGHT;
    g_uvc_ai_app.frame_counter = 0U;
    g_uvc_ai_app.frame_pending = RT_FALSE;
    g_uvc_ai_app.ai_ready = RT_FALSE;
    g_uvc_ai_app.ai_failed = RT_FALSE;
    g_uvc_ai_app.started = RT_TRUE;
    g_uvc_ai_app.session_gen++;
    g_uvc_ai_app.consecutive_errors = 0U;
    uvc_ai_fps_reset_locked();
    uvc_ai_app_reset_tracking_locked();
    rt_hw_interrupt_enable(level);

    uvc_display_set_overlay_callback(uvc_ai_app_overlay_cb, RT_NULL);
    rt_event_send(&g_uvc_ai_worker_wake, UVC_AI_WORK_AVAILABLE);

    USB_LOG_INFO("AI app started (async)\r\n");
    return RT_EOK;
}

void uvc_ai_app_process_frame(const struct usbh_videoframe *frame, rt_tick_t frame_tick)
{
    if (frame == RT_NULL || frame->frame_buf == RT_NULL ||
        frame->frame_format != USBH_VIDEO_FORMAT_UNCOMPRESSED ||
        frame->frame_size < UVC_AI_FRAME_BYTES) {
        return;
    }

    rt_base_t level = rt_hw_interrupt_disable();
    if (!g_uvc_ai_app.started) {
        rt_hw_interrupt_enable(level);
        return;
    }

    g_uvc_ai_app.frame_counter++;
    if (!g_uvc_ai_app.ai_ready) {
        rt_bool_t retry_init = g_uvc_ai_app.ai_failed && !g_uvc_ai_app.worker_busy &&
            (g_uvc_ai_app.desired_mode == UVC_AI_MODE_STEEL_BALL) && sdcard_is_mounted() &&
            ((rt_tick_t)(rt_tick_get() - g_uvc_ai_app.last_retry_tick) >=
             rt_tick_from_millisecond(500));
        rt_hw_interrupt_enable(level);
        if (retry_init) {
            rt_event_send(&g_uvc_ai_worker_wake, UVC_AI_WORK_AVAILABLE);
        }
        return;
    }

    if (g_uvc_ai_app.frame_copying ||
        (g_uvc_ai_app.frame_counter % UVC_AI_FRAMES_TO_SKIP) != 0U) {
        rt_hw_interrupt_enable(level);
        return;
    }

    /* One slot belongs to inference. The other holds only the latest input. */
    uint8_t slot = g_uvc_ai_app.worker_slot ^ 1U;
    uint32_t gen = g_uvc_ai_app.session_gen;
    rt_bool_t already_pending = g_uvc_ai_app.frame_pending;
    g_uvc_ai_app.frame_copying = RT_TRUE;
    rt_hw_interrupt_enable(level);

    memcpy(g_uvc_ai_worker_frames[slot], frame->frame_buf, UVC_AI_FRAME_BYTES);

    level = rt_hw_interrupt_disable();
    g_uvc_ai_app.frame_copying = RT_FALSE;
    rt_bool_t wake = g_uvc_ai_app.started && g_uvc_ai_app.ai_ready &&
                     (g_uvc_ai_app.session_gen == gen);
    if (wake) {
        g_uvc_ai_app.frame_tick = frame_tick;
        g_uvc_ai_app.frame_pending = RT_TRUE;
        if (already_pending) {
            g_uvc_ai_app.dropped_busy_frames++;
        }
    }
    rt_hw_interrupt_enable(level);

    if (wake) {
        rt_event_send(&g_uvc_ai_worker_wake, UVC_AI_WORK_AVAILABLE);
    }
}

int uvc_ai_app_stop(void)
{
    uvc_display_set_overlay_callback(RT_NULL, RT_NULL);

    rt_base_t level = rt_hw_interrupt_disable();
    if (!g_uvc_ai_app.started) {
        rt_hw_interrupt_enable(level);
        return RT_EOK;
    }

    g_uvc_ai_app.started = RT_FALSE;
    g_uvc_ai_app.ai_ready = RT_FALSE;
    g_uvc_ai_app.ai_failed = RT_FALSE;
    g_uvc_ai_app.frame_pending = RT_FALSE;
    g_uvc_ai_app.session_gen++;
    uvc_ai_fps_reset_locked();
    uvc_ai_app_reset_tracking_locked();
    rt_hw_interrupt_enable(level);

    rt_event_send(&g_uvc_ai_worker_wake, UVC_AI_WORK_AVAILABLE);
    USB_LOG_INFO("AI app stopping (async)\r\n");
    return RT_EOK;
}

static int uvc_ai_app_reload_steel_ball(void)
{
    rt_base_t level;
    rt_bool_t started;

    level = rt_hw_interrupt_disable();
    started = g_uvc_ai_app.started;
    if (started && (g_uvc_ai_app.desired_mode == UVC_AI_MODE_STEEL_BALL)) {
        g_uvc_ai_app.reload_req = RT_TRUE;
        g_uvc_ai_app.ai_ready = RT_FALSE;
        g_uvc_ai_app.ai_failed = RT_FALSE;
        g_uvc_ai_app.frame_pending = RT_FALSE;
        g_uvc_ai_app.session_gen++;
        uvc_ai_fps_reset_locked();
        uvc_ai_app_reset_tracking_locked();
        rt_hw_interrupt_enable(level);
        if (g_uvc_ai_worker_initialized) {
            rt_event_send(&g_uvc_ai_worker_wake, UVC_AI_WORK_AVAILABLE);
        }
        USB_LOG_INFO("Steel ball model reload requested\r\n");
    } else {
        rt_hw_interrupt_enable(level);
        rt_mutex_take(&g_uvc_ai_lifecycle, RT_WAITING_FOREVER);
        uvc_ai_invalidate_steel_ball_cache();
        rt_mutex_release(&g_uvc_ai_lifecycle);
        USB_LOG_INFO("Steel ball model cache invalidated\r\n");
    }
    return RT_EOK;
}

static int uvc_ai_app_wait_settled(uint32_t timeout_ms)
{
    rt_tick_t start = rt_tick_get();
    while ((rt_tick_t)(rt_tick_get() - start) < rt_tick_from_millisecond(timeout_ms)) {
        rt_base_t level = rt_hw_interrupt_disable();
        rt_bool_t done = !g_uvc_ai_app.started ||
                         (!g_uvc_ai_app.reload_req &&
                          (g_uvc_ai_app.active_mode == g_uvc_ai_app.desired_mode) &&
                          (g_uvc_ai_app.ai_ready || g_uvc_ai_app.ai_failed));
        rt_bool_t failed = g_uvc_ai_app.ai_failed;
        rt_hw_interrupt_enable(level);
        if (done) {
            return failed ? -RT_ERROR : RT_EOK;
        }
        rt_thread_mdelay(10);
    }
    return -RT_ETIMEOUT;
}

static int cmd_ai_mode(int argc, char **argv)
{
    if (argc >= 2) {
        if (strcmp(argv[1], "steel_ball") == 0 || strcmp(argv[1], "ball") == 0) {
            int r = uvc_ai_app_set_mode(UVC_AI_MODE_STEEL_BALL);
            if (r == RT_EOK && g_uvc_ai_app.started)
                r = uvc_ai_app_wait_settled(3000);
            rt_kprintf("ai_mode=steel_ball ret=%d\n", r);
            return r;
        }
        if (strcmp(argv[1], "fatigue") == 0 || strcmp(argv[1], "eye") == 0) {
            int r = uvc_ai_app_set_mode(UVC_AI_MODE_FATIGUE);
            if (r == RT_EOK && g_uvc_ai_app.started)
                r = uvc_ai_app_wait_settled(3000);
            rt_kprintf("ai_mode=fatigue ret=%d\n", r);
            return r;
        }
        if (strcmp(argv[1], "reload") == 0) {
            int r = uvc_ai_app_reload_steel_ball();
            if (r == RT_EOK && g_uvc_ai_app.started)
                r = uvc_ai_app_wait_settled(3000);
            rt_kprintf("ai_mode=reload ret=%d\n", r);
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
MSH_CMD_EXPORT_ALIAS(cmd_ai_mode, ai_mode, Switch/query AI detection mode [fatigue|steel_ball|reload|status]);

static int cmd_ai_status(int argc, char **argv)
{
    uvc_ai_telemetry_t tel;
    (void)argc;
    (void)argv;
    uvc_ai_app_get_telemetry(&tel);
    rt_kprintf("AI mode=%s active=%s running=%d ready=%d busy=%d failed=%d inf=%lu err=%lu sw=%lu drop=%lu npu_tmo=%lu ai_fps=%.1f inf_ms=%.1f det=%u recoveries=%lu\n",
               (tel.mode == UVC_AI_MODE_STEEL_BALL) ? "steel_ball" : "fatigue",
               (g_uvc_ai_app.active_mode == UVC_AI_MODE_STEEL_BALL) ? "steel_ball" : "fatigue",
               tel.running ? 1 : 0,
               tel.ai_ready ? 1 : 0,
               g_uvc_ai_app.worker_busy ? 1 : 0,
               g_uvc_ai_app.ai_failed ? 1 : 0,
               (unsigned long)g_uvc_ai_app.infer_count,
               (unsigned long)g_uvc_ai_app.infer_errors,
               (unsigned long)g_uvc_ai_app.mode_switches,
               (unsigned long)g_uvc_ai_app.dropped_busy_frames,
               (unsigned long)uvc_ai_get_npu_timeout_count(),
               (double)tel.ai_fps,
               (double)tel.inference_ms,
               (unsigned)tel.det_count,
               (unsigned long)g_uvc_ai_app.recoveries);
    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_ai_status, ai_status, Show AI lifecycle inference and watchdog counters);

static rt_bool_t uvc_ai_parse_simple_float(const char *s, float *out)
{
    float val = 0.0f;
    float frac = 0.1f;
    rt_bool_t neg = RT_FALSE;
    rt_bool_t has_digit = RT_FALSE;

    if ((s == RT_NULL) || (out == RT_NULL)) {
        return RT_FALSE;
    }
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    if (*s == '-') {
        neg = RT_TRUE;
        s++;
    } else if (*s == '+') {
        s++;
    }
    while (*s >= '0' && *s <= '9') {
        val = val * 10.0f + (float)(*s - '0');
        has_digit = RT_TRUE;
        s++;
    }
    if (*s == '.') {
        s++;
        while (*s >= '0' && *s <= '9') {
            val += (float)(*s - '0') * frac;
            frac *= 0.1f;
            has_digit = RT_TRUE;
            s++;
        }
    }
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    if (!has_digit || *s != '\0') {
        return RT_FALSE;
    }
    *out = neg ? -val : val;
    return isfinite(*out) ? RT_TRUE : RT_FALSE;
}

static int cmd_ai_thresh(int argc, char **argv)
{
    if (argc >= 2) {
        float v = 0.0f;
        if (!uvc_ai_parse_simple_float(argv[1], &v) || v < 0.15f || v > 0.95f) {
            rt_kprintf("Usage: ai_thresh [0.15..0.95]\n");
            return -RT_EINVAL;
        }
        uvc_ai_set_conf_threshold(v);
    }
    rt_kprintf("ai_thresh=%.2f mode=%s\n",
               (double)uvc_ai_get_conf_threshold(),
               (uvc_ai_app_get_mode() == UVC_AI_MODE_STEEL_BALL) ? "steel_ball" : "fatigue");
    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_ai_thresh, ai_thresh, Get or set steel-ball confidence threshold [0.15..0.95]);

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

static void fill_synth_steel_balls_dense_yuyv(uint8_t *yuyv, uint32_t count, int32_t frame_step)
{
    uint32_t y;
    uint32_t x;
    uint32_t b;
    uint32_t cols;
    uint32_t rows;
    float cell_w;
    float cell_h;

    if (count < 1U) count = 1U;
    if (count > 48U) count = 48U;

    /* Background gradient */
    for (y = 0U; y < UVC_AI_CAMERA_HEIGHT; y++) {
        uint8_t *row = yuyv + y * (UVC_AI_CAMERA_WIDTH * 2U);
        for (x = 0U; x < UVC_AI_CAMERA_WIDTH; x += 2U) {
            int32_t bg0 = 132 + (((int32_t)x * 36) / 320) - 18;
            int32_t bg1 = 132 + (((int32_t)(x + 1U) * 36) / 320) - 18;
            row[x * 2U + 0U] = (uint8_t)bg0;
            row[x * 2U + 1U] = 128U;
            row[x * 2U + 2U] = (uint8_t)bg1;
            row[x * 2U + 3U] = 128U;
        }
    }

    cols = (count <= 16U) ? 4U : ((count <= 32U) ? 8U : 8U);
    rows = (count + cols - 1U) / cols;
    if (rows < 1U) rows = 1U;
    cell_w = (float)(UVC_AI_CAMERA_WIDTH - 24U) / (float)cols;
    cell_h = (float)(UVC_AI_CAMERA_HEIGHT - 24U) / (float)rows;

    for (b = 0U; b < count; b++) {
        uint32_t col = b % cols;
        uint32_t row_idx = b / cols;
        float max_r = 0.42f * fminf(cell_w, cell_h);
        float r = 10.0f + (float)((b * 5U) % 9U);
        float cx;
        float cy;
        float r2;
        int32_t x0, y0, x1, y1, py, px;

        if (r > max_r) r = max_r;
        if (r < 7.0f) r = 7.0f;
        r2 = r * r;

        cx = 12.0f + ((float)col + 0.5f) * cell_w + (float)((int32_t)((b * 3U) % 5U) - 2) + (float)(frame_step * 2);
        cy = 12.0f + ((float)row_idx + 0.5f) * cell_h + (float)((int32_t)((b * 7U) % 5U) - 2) + (float)frame_step;
        if (cx < r + 2.0f) cx = r + 2.0f;
        if (cx > (float)UVC_AI_CAMERA_WIDTH - r - 2.0f) cx = (float)UVC_AI_CAMERA_WIDTH - r - 2.0f;
        if (cy < r + 2.0f) cy = r + 2.0f;
        if (cy > (float)UVC_AI_CAMERA_HEIGHT - r - 2.0f) cy = (float)UVC_AI_CAMERA_HEIGHT - r - 2.0f;

        x0 = (int32_t)floorf(cx - r);
        y0 = (int32_t)floorf(cy - r);
        x1 = (int32_t)ceilf(cx + r);
        y1 = (int32_t)ceilf(cy + r);
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 >= (int32_t)UVC_AI_CAMERA_WIDTH) x1 = (int32_t)UVC_AI_CAMERA_WIDTH - 1;
        if (y1 >= (int32_t)UVC_AI_CAMERA_HEIGHT) y1 = (int32_t)UVC_AI_CAMERA_HEIGHT - 1;

        for (py = y0; py <= y1; py++) {
            uint8_t *dst_row = yuyv + (uint32_t)py * (UVC_AI_CAMERA_WIDTH * 2U);
            float dy = (float)py - cy;
            for (px = x0; px <= x1; px++) {
                float dx = (float)px - cx;
                float d2 = dx * dx + dy * dy;
                if (d2 <= r2) {
                    float nx = dx / r;
                    float ny = dy / r;
                    float nz = sqrtf(fmaxf(0.0f, 1.0f - nx * nx - ny * ny));
                    float diff = fmaxf(0.0f, nx * (-0.40f) + ny * (-0.50f) + nz * 0.76f);
                    float spec_base = fmaxf(0.0f, nx * (-0.25f) + ny * (-0.32f) + nz * 0.91f);
                    float spec = powf(spec_base, 24.0f);
                    int32_t val = (int32_t)(42.0f + 150.0f * diff + 225.0f * spec);
                    if (val < 0) val = 0;
                    if (val > 255) val = 255;
                    dst_row[(uint32_t)px * 2U] = (uint8_t)val;
                }
            }
        }
    }
}

static uint32_t ai_selftest_count_nonfinite(const uvc_ai_result_t *r)
{
    uint32_t bad = 0U;
    uint8_t i;
    if (r == RT_NULL) {
        return 0U;
    }
    if (!isfinite(r->inference_ms)) {
        bad++;
    }
    for (i = 0U; i < r->count && i < UVC_AI_MAX_PREDICTIONS; i++) {
        if (!isfinite(r->conf[i])) {
            bad++;
        }
    }
    return bad;
}

typedef struct {
    struct rt_semaphore done;
    int ret;
    rt_bool_t dense;
    rt_bool_t tracks;
    uint32_t dense_count;
} ai_selftest_ctx_t;

static void ai_selftest_add_ball(uvc_ai_result_t *r, int16_t x, int16_t y, float score)
{
    unsigned int i = r->count++;
    r->bbox_int16[4U * i] = x - 6;
    r->bbox_int16[4U * i + 1U] = y - 6;
    r->bbox_int16[4U * i + 2U] = x + 6;
    r->bbox_int16[4U * i + 3U] = y + 6;
    r->conf[i] = score;
}

static uint16_t ai_selftest_track_at(const uvc_ai_result_t *r, int x, int y)
{
    for (unsigned int i = 0U; i < r->count; i++) {
        const int16_t *box = &r->bbox_int16[4U * i];
        if (x >= box[0] && x <= box[2] && y >= box[1] && y <= box[3]) {
            return r->track_id[i];
        }
    }
    return 0U;
}

static int ai_selftest_tracks(void)
{
    sb_tracker_t tracker;
    uvc_ai_result_t high = {0}, low = {0}, out;

    /* The nearest single pair would leave the second track unmatched. */
    sb_tracker_reset(&tracker);
    ai_selftest_add_ball(&high, 80, 100, 0.9f);
    ai_selftest_add_ball(&high, 103, 100, 0.9f);
    sb_tracker_update(&tracker, &high, &low, 0U, &out);
    uint16_t a = ai_selftest_track_at(&out, 80, 100);
    uint16_t b = ai_selftest_track_at(&out, 103, 100);
    high.count = 0U;
    ai_selftest_add_ball(&high, 88, 100, 0.9f);
    ai_selftest_add_ball(&high, 64, 100, 0.9f);
    sb_tracker_update(&tracker, &high, &low, 100U, &out);
    if (!a || !b || a == b || out.count != 2U ||
        ai_selftest_track_at(&out, 69, 100) != a || ai_selftest_track_at(&out, 93, 100) != b) {
        rt_kprintf("ai_selftest tracks: global association failed\n");
        return -RT_ERROR;
    }

    sb_tracker_reset(&tracker);
    for (int f = 0; f < 11; f++) {
        int16_t x_a = 85 + 7 * f;
        int16_t x_b = 155 - 7 * f;
        high.count = low.count = 0U;
        if (f & 1) ai_selftest_add_ball(&high, x_b, 112, 0.88f);
        if (f == 4) ai_selftest_add_ball(&low, x_a, 100, 0.38f);
        else if (f != 6) ai_selftest_add_ball(&high, x_a, 100, 0.88f);
        if (!(f & 1)) ai_selftest_add_ball(&high, x_b, 112, 0.88f);
        sb_tracker_update(&tracker, &high, &low, 100U * (uint32_t)f, &out);

        uint16_t found_a = ai_selftest_track_at(&out, x_a, 100);
        uint16_t found_b = ai_selftest_track_at(&out, x_b, 112);
        if (f == 0) {
            a = found_a;
            b = found_b;
        }
        if (!a || !b || a == b || out.count != 2U || found_a != a || found_b != b) {
            rt_kprintf("ai_selftest tracks: identity lost at frame %d\n", f);
            return -RT_ERROR;
        }
    }

    high.count = low.count = 0U;
    for (uint32_t f = 11U; f <= 14U; f++)
        sb_tracker_update(&tracker, &high, &low, 100U * f, &out);
    if (out.count != 0U) {
        rt_kprintf("ai_selftest tracks: stale tracks did not expire\n");
        return -RT_ERROR;
    }

    sb_tracker_reset(&tracker);
    ai_selftest_add_ball(&high, 70, 120, 0.80f);
    sb_tracker_update(&tracker, &high, &low, 0U, &out);
    a = ai_selftest_track_at(&out, 70, 120);
    if (!a) {
        rt_kprintf("ai_selftest tracks: single-ball first detection failed\n");
        return -RT_ERROR;
    }
    int before = 0, after = 0;
    for (uint32_t f = 1U; f <= 5U; f++) {
        int x = 70 + 28 * (int)f;
        high.count = 0U;
        ai_selftest_add_ball(&high, (int16_t)x, 120, 0.80f);
        sb_tracker_update(&tracker, &high, &low, 100U * f, &out);
        if (out.count != 1U || out.track_id[0] != a || out.single_track_vx <= 0) {
            rt_kprintf("ai_selftest tracks: fast single-ball identity lost at frame %lu\n",
                       (unsigned long)f);
            return -RT_ERROR;
        }
        int16_t projected[4];
        sb_tracker_project_single(&out, 70U, projected);
        int expected = x + 20;
        before += abs(expected - (out.bbox_int16[0] + out.bbox_int16[2]) / 2);
        after += abs(expected - (projected[0] + projected[2]) / 2);
    }
    if (after * 2 >= before) {
        rt_kprintf("ai_selftest tracks: single-ball projection lag (%d -> %d px)\n", before, after);
        return -RT_ERROR;
    }
    rt_kprintf("ai_selftest tracks: PASS global assignment, crossing, weak detection, occlusion, expiry\n");
    rt_kprintf("ai_selftest tracks: PASS fast single ball, 70 ms render lag %d -> %d px over 5 samples\n",
               before, after);
    return RT_EOK;
}

static void ai_selftest_thread_entry(void *param)
{
    ai_selftest_ctx_t *ctx = (ai_selftest_ctx_t *)param;
    uvc_ai_config_t cfg;
    uvc_ai_result_t res;
    int ret;
    uint8_t i;

    if (ctx->tracks) {
        ctx->ret = ai_selftest_tracks();
        rt_sem_release(&ctx->done);
        return;
    }

    cfg.initialized = 1U;
    cfg.src_width = UVC_AI_CAMERA_WIDTH;
    cfg.src_height = UVC_AI_CAMERA_HEIGHT;

    if (ctx->dense) {
        uvc_ai_mode_t saved_mode = uvc_ai_get_mode();
        uvc_ai_result_t low_res;
        uvc_ai_result_t tracked;
        uvc_display_overlay_info_t ovl_info;
        uint32_t rounds = 5U;
        uint32_t total_nonfinite = 0U;
        float max_ms = 0.0f;
        float sum_ms = 0.0f;
        uint32_t max_high = 0U;
        uint32_t max_low = 0U;
        uint32_t max_tracked = 0U;
        uint32_t f;

        uvc_ai_set_mode(UVC_AI_MODE_STEEL_BALL);
        ret = uvc_ai_init(&cfg);
        if (ret != RT_EOK) {
            uvc_ai_set_mode(saved_mode);
            rt_kprintf("ai_selftest dense: init failed (%d)\n", ret);
            ctx->ret = ret;
            rt_sem_release(&ctx->done);
            return;
        }

        sb_tracker_reset(&g_sb_tracker);
        rt_memset(&ovl_info, 0, sizeof(ovl_info));
        ovl_info.framebuffer = (uint16_t *)g_uvc_ai_worker_frames[0];
        ovl_info.lcd_width = UVC_AI_CAMERA_WIDTH;
        ovl_info.lcd_height = UVC_AI_CAMERA_HEIGHT;
        ovl_info.framebuffer_stride = UVC_AI_CAMERA_WIDTH;
        ovl_info.src_width = UVC_AI_CAMERA_WIDTH;
        ovl_info.src_height = UVC_AI_CAMERA_HEIGHT;
        ovl_info.dst_width = UVC_AI_CAMERA_WIDTH;
        ovl_info.dst_height = UVC_AI_CAMERA_HEIGHT;
        ovl_info.dst_y_offset = 0U;

        rt_kprintf("ai_selftest dense: synth_balls=%lu thresh=%.2f rounds=%lu\n",
                   (unsigned long)ctx->dense_count,
                   (double)uvc_ai_get_conf_threshold(),
                   (unsigned long)rounds);

        for (f = 0U; f < rounds; f++) {
            rt_tick_t t0;
            rt_tick_t t1;
            uint32_t now_ms;
            uint32_t bad;
            float max_det_c = 0.0f;

            fill_synth_steel_balls_dense_yuyv(g_uvc_ai_worker_frames[0], ctx->dense_count, (int32_t)f);
            t0 = rt_tick_get();
            ret = uvc_ai_process_yuyv_ex(g_uvc_ai_worker_frames[0], UVC_AI_FRAME_BYTES, &res, &low_res);
            if (ret != RT_EOK) {
                rt_kprintf("ai_selftest dense: round %lu infer failed (%d)\n",
                           (unsigned long)(f + 1U), ret);
                break;
            }
            now_ms = (uint32_t)(((uint64_t)rt_tick_get() * 1000ULL) / (uint64_t)RT_TICK_PER_SECOND);
            sb_tracker_update(&g_sb_tracker, &res, &low_res, now_ms, &tracked);
            /* Reuse frame buffer as synthetic 320x240 RGB565 surface to stress overlay drawing + rt_snprintf. */
            (void)uvc_ai_app_draw_detections(&ovl_info, &tracked, UVC_AI_MODE_STEEL_BALL,
                                             10.0f, 0U, &max_det_c, RT_NULL);
            t1 = rt_tick_get();

            bad = ai_selftest_count_nonfinite(&res) +
                  ai_selftest_count_nonfinite(&low_res) +
                  ai_selftest_count_nonfinite(&tracked);
            total_nonfinite += bad;

            if (res.inference_ms > max_ms) max_ms = res.inference_ms;
            sum_ms += res.inference_ms;
            if (res.count > max_high) max_high = res.count;
            if (low_res.count > max_low) max_low = low_res.count;
            if (tracked.count > max_tracked) max_tracked = tracked.count;

            rt_kprintf("  r%lu: inf=%.1fms total=%lums high=%u low=%u tracked=%u nonfinite=%lu\n",
                       (unsigned long)(f + 1U),
                       (double)res.inference_ms,
                       (unsigned long)((uint32_t)(t1 - t0) * (1000U / RT_TICK_PER_SECOND)),
                       (unsigned)res.count, (unsigned)low_res.count,
                       (unsigned)tracked.count, (unsigned long)bad);
        }

        uvc_ai_deinit();
        sb_tracker_reset(&g_sb_tracker);
        uvc_ai_set_mode(saved_mode);

        if (ret == RT_EOK) {
            rt_kprintf("ai_selftest dense: DONE avg=%.1fms max=%.1fms max_hi=%lu max_lo=%lu max_trk=%lu nonfinite=%lu\n",
                       (double)(sum_ms / (float)rounds), (double)max_ms,
                       (unsigned long)max_high, (unsigned long)max_low,
                       (unsigned long)max_tracked, (unsigned long)total_nonfinite);
        }
        ctx->ret = (ret == RT_EOK && total_nonfinite == 0U) ? RT_EOK : -RT_ERROR;
        rt_sem_release(&ctx->done);
        return;
    }

    uvc_ai_set_mode(uvc_ai_app_get_mode());
    ret = uvc_ai_init(&cfg);
    if (ret != RT_EOK) {
        rt_kprintf("ai_selftest: init failed (%d)\n", ret);
        ctx->ret = ret;
        rt_sem_release(&ctx->done);
        return;
    }

    fill_synth_steel_ball_yuyv(g_uvc_ai_worker_frames[0]);
    ret = uvc_ai_process_yuyv(g_uvc_ai_worker_frames[0], UVC_AI_FRAME_BYTES, &res);
    if (ret == RT_EOK && uvc_ai_get_mode() == UVC_AI_MODE_STEEL_BALL) {
        uvc_ai_result_t sample;
        for (unsigned int trial = 0; trial < 4U; trial++) {
            memset(g_uvc_ai_worker_frames[0], 128, UVC_AI_FRAME_BYTES);
            ret = uvc_ai_process_yuyv(g_uvc_ai_worker_frames[0], UVC_AI_FRAME_BYTES, &sample);
            if (ret != RT_EOK || sample.count != 0U) {
                rt_kprintf("ai_selftest: blank frame check failed (trial=%u)\n", trial + 1U);
                ret = -RT_ERROR;
                break;
            }
            fill_synth_steel_ball_yuyv(g_uvc_ai_worker_frames[0]);
            ret = uvc_ai_process_yuyv(g_uvc_ai_worker_frames[0], UVC_AI_FRAME_BYTES, &sample);
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

    rt_memset(&ctx, 0, sizeof(ctx));
    ctx.tracks = argc >= 2 && strcmp(argv[1], "tracks") == 0;
    if (argc >= 2 && strcmp(argv[1], "dense") == 0) {
        int cnt = (argc >= 3) ? atoi(argv[2]) : 28;
        if (cnt < 1) cnt = 1;
        if (cnt > 48) cnt = 48;
        ctx.dense = RT_TRUE;
        ctx.dense_count = (uint32_t)cnt;
    }

    if (rt_mutex_take(&g_uvc_ai_lifecycle, RT_WAITING_NO) != RT_EOK) {
        rt_kprintf("ai_selftest: model busy\n");
        return -RT_EBUSY;
    }
    if (g_uvc_ai_app.started || g_hw_ai_ready ||
        g_uvc_ai_app.worker_busy || g_uvc_ai_app.frame_copying) {
        rt_mutex_release(&g_uvc_ai_lifecycle);
        rt_kprintf("ai_selftest: busy (camera running)\n");
        return -RT_EBUSY;
    }

    rt_sem_init(&ctx.done, "aiselft", 0, RT_IPC_FLAG_FIFO);
    ctx.ret = -RT_ERROR;

    tid = rt_thread_create("ai_test", ai_selftest_thread_entry, &ctx,
                           24576, UVC_AI_WORKER_PRIORITY, UVC_AI_WORKER_TICK);
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
MSH_CMD_EXPORT_ALIAS(cmd_ai_selftest, ai_selftest, Run AI self-test [dense [count]|tracks]);
