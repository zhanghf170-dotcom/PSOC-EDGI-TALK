#include <rtthread.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#include "uvc_ai.h"
#include "model.h"
#if defined(__has_include)
#if __has_include("steel_ball_model.h")
#include "steel_ball_model.h"
#endif
#endif
#ifndef STEEL_BALL_MODEL_MAX_BIN_SIZE
#define STEEL_BALL_MODEL_MAX_BIN_SIZE (6U * 1024U * 1024U)
#endif
#ifndef STEEL_BALL_ARENA_SIZE
#define STEEL_BALL_ARENA_SIZE         (885760U)
#endif
#ifndef STEEL_BALL_DFL_REG_MAX
#define STEEL_BALL_DFL_REG_MAX        (16U)
#endif
#ifndef STEEL_BALL_NUM_CLASSES
#define STEEL_BALL_NUM_CLASSES        (1U)
#endif
#include "mtb_ml.h"
#include "usb_config.h"
#include "sd_assets.h"

extern uint8_t *ml_shared_get_arena(uint32_t *size_out);
extern uint32_t mtb_ml_ethosu_get_timeout_count(void);

#undef USB_DBG_TAG
#define USB_DBG_TAG "uvc_ai"
#include "usb_log.h"

#define UVC_AI_HALF(x)      ((x) * 0.5f)
#define UVC_AI_ROUND_FACTOR (0.5f)
#define UVC_AI_ITCM_SECTION __attribute__((section(".cy_itcm")))
#define UVC_AI_DTCM_SECTION __attribute__((section(".cy_dtcm")))
#define UVC_AI_ML_INPUT_SECTION __attribute__((section(".cy_ml_input_data")))
#define UVC_AI_ALIGN_32 __attribute__((aligned(32)))
#define UVC_AI_PAD_VALUE_U8 ((uint8_t)0)
#define UVC_AI_SB_PAD_VALUE_U8 ((uint8_t)114U)
#define UVC_AI_SB_PAD_VALUE_S8 ((uint8_t)0xF2U) /* (int8_t)(114 - 128) = -14 */
#define UVC_AI_SRC_STRIDE_BYTES (UVC_AI_CAMERA_WIDTH * 2U)
#define UVC_AI_SRC_PAIR_COUNT (UVC_AI_CAMERA_WIDTH / 2U)
#define UVC_AI_FRAME_BYTES (UVC_AI_SRC_STRIDE_BYTES * UVC_AI_CAMERA_HEIGHT)
#define UVC_AI_TOP_RGB888_BYTES (UVC_AI_CAMERA_WIDTH * UVC_AI_CAMERA_HEIGHT * 3U)
#define UVC_AI_PAD_RGB888_BYTES ((UVC_AI_IMAGE_HEIGHT - UVC_AI_CAMERA_HEIGHT) * UVC_AI_IMAGE_WIDTH * 3U)
#define UVC_AI_MODEL_HEADER_WORD0 (0x00000024UL)
#define UVC_AI_MODEL_HEADER_WORD1 (0x334c4654UL)
#define SB_MAX_CANDIDATES 160U
#define SB_MAX_VERIFY_PER_FRAME 16U

static UVC_AI_ML_INPUT_SECTION UVC_AI_ALIGN_32 uint8_t g_model_input_u8[IMAI_DATA_IN_COUNT];
static UVC_AI_ML_INPUT_SECTION UVC_AI_ALIGN_32 uint8_t g_sb_decode_scratch[16384];
static UVC_AI_DTCM_SECTION float g_model_output[IMAI_DATA_OUT_COUNT];

static uint8_t g_ai_initialized;
static uvc_ai_mode_t g_ai_active_mode = UVC_AI_MODE_STEEL_BALL;
static uvc_ai_mode_t g_ai_init_mode = UVC_AI_MODE_STEEL_BALL;
static float g_steel_ball_conf_thresh = 0.55f;
static mtb_ml_model_t *g_steel_ball_model;
static uint8_t g_sb_fast_quant_input;
static uint8_t g_sb_hyperram_loaded;
static uint32_t g_sb_loaded_mount_gen;
static uint32_t g_sb_model_bin_size;
static uint16_t g_sb_input_w = UVC_AI_IMAGE_WIDTH;
static uint16_t g_sb_input_h = UVC_AI_IMAGE_HEIGHT;
static uint32_t g_sb_input_bytes = IMAI_DATA_IN_COUNT;
static __attribute__((section(".m33_m55_shared_hyperram"), aligned(32)))
    uint8_t g_steel_ball_hyperram_bin[(STEEL_BALL_MODEL_MAX_BIN_SIZE + 31U) & ~31U];

typedef struct {
    int8_t *box_ptr;
    int8_t *cls_ptr;
    int32_t box_zp;
    float box_scale;
    int32_t cls_zp;
    float cls_scale;
    uint16_t grid_h;
    uint16_t grid_w;
    float stride_x;
    float stride_y;
    uint8_t cls_is_prob;
} steel_ball_scale_desc_t;

static steel_ball_scale_desc_t g_sb_scales[3];
static UVC_AI_DTCM_SECTION float g_sb_dfl_exp_lut[3][256];
static uint16_t g_src_width;
static uint16_t g_src_height;
static uint64_t g_last_npu_cycles;
static uint32_t g_last_score_count;
static uint8_t g_restore_unalign_trp;
static uint32_t g_saved_ccr;
static uint8_t g_uvc_ai_yuv_lut_ready;
static rt_tick_t g_last_ai_log_tick;
/* Studio-Swing BT.601 LUTs (16..235) */
static UVC_AI_DTCM_SECTION int32_t g_uvc_ai_y_lut[256];
static UVC_AI_DTCM_SECTION int32_t g_uvc_ai_u_to_b_lut[256];
static UVC_AI_DTCM_SECTION int32_t g_uvc_ai_u_to_g_lut[256];
static UVC_AI_DTCM_SECTION int32_t g_uvc_ai_v_to_r_lut[256];
static UVC_AI_DTCM_SECTION int32_t g_uvc_ai_v_to_g_lut[256];
/* Full-Range BT.601 LUTs (0..255) */
static UVC_AI_DTCM_SECTION int32_t g_uvc_ai_y_lut_full[256];
static UVC_AI_DTCM_SECTION int32_t g_uvc_ai_u_to_b_lut_full[256];
static UVC_AI_DTCM_SECTION int32_t g_uvc_ai_u_to_g_lut_full[256];
static UVC_AI_DTCM_SECTION int32_t g_uvc_ai_v_to_r_lut_full[256];
static UVC_AI_DTCM_SECTION int32_t g_uvc_ai_v_to_g_lut_full[256];

extern uint8_t __cy_ml_model_data_start__[];
extern uint8_t __cy_ml_model_data_end__[];
extern const uint8_t __cy_ml_model_data_load_start__[];
extern const uint8_t __cy_ml_model_data_load_end__[];

static inline uint8_t uvc_ai_sat_to_u8(int32_t value)
{
#if defined(__ARM_FEATURE_DSP) || defined(__ARM_FEATURE_MVE)
    return (uint8_t)__USAT(value, 8U);
#else
    if (value < 0) {
        value = 0;
    }

    if (value > 255) {
        value = 255;
    }

    return (uint8_t)value;
#endif
}

static inline uint32_t uvc_ai_load_u32_unaligned(const uint8_t *src)
{
#if defined(__UNALIGNED_UINT32_READ)
    return __UNALIGNED_UINT32_READ(src);
#else
    uint32_t packed;
    memcpy(&packed, src, sizeof(packed));
    return packed;
#endif
}

static inline int16_t uvc_ai_clamp_i16(int32_t value, int32_t min_v, int32_t max_v)
{
    if (value < min_v) {
        value = min_v;
    }

    if (value > max_v) {
        value = max_v;
    }

    return (int16_t)value;
}

static void uvc_ai_prepare_yuv_lut(void)
{
    uint32_t i;

    if (g_uvc_ai_yuv_lut_ready) {
        return;
    }

    for (i = 0U; i < 256U; i++) {
        int32_t y_studio = (int32_t)i - 16;
        int32_t uv = (int32_t)i - 128;

        if (y_studio < 0) {
            y_studio = 0;
        }

        /* Studio-Swing BT.601 */
        g_uvc_ai_y_lut[i] = 298 * y_studio;
        g_uvc_ai_u_to_b_lut[i] = 516 * uv;
        g_uvc_ai_u_to_g_lut[i] = -100 * uv;
        g_uvc_ai_v_to_r_lut[i] = 409 * uv;
        g_uvc_ai_v_to_g_lut[i] = -208 * uv;

        /* Full-Range BT.601 */
        g_uvc_ai_y_lut_full[i] = 256 * (int32_t)i;
        g_uvc_ai_u_to_b_lut_full[i] = 454 * uv;
        g_uvc_ai_u_to_g_lut_full[i] = -88 * uv;
        g_uvc_ai_v_to_r_lut_full[i] = 359 * uv;
        g_uvc_ai_v_to_g_lut_full[i] = -183 * uv;
    }

    g_uvc_ai_yuv_lut_ready = 1U;
}

static UVC_AI_ITCM_SECTION uint8_t uvc_ai_probe_full_range_yuyv(const uint8_t *yuyv)
{
    uint32_t off;
    uint8_t y_min = 255U;
    uint8_t y_max = 0U;

    /* Deterministically sample 320 Y pixels across 320x240 YUYV (step 480 bytes, even offset = Y). */
    for (off = 0U; off < UVC_AI_FRAME_BYTES; off += 480U) {
        uint8_t yv = yuyv[off];
        if (yv < y_min) {
            y_min = yv;
        }
        if (yv > y_max) {
            y_max = yv;
        }
    }

    return ((y_min < 12U) || (y_max > 240U)) ? 1U : 0U;
}

static void uvc_ai_sync_model_blob_from_flash(void)
{
    uint8_t *runtime = __cy_ml_model_data_start__;
    const uint8_t *flash = __cy_ml_model_data_load_start__;
    uintptr_t runtime_start = (uintptr_t)__cy_ml_model_data_start__;
    uintptr_t runtime_end = (uintptr_t)__cy_ml_model_data_end__;
    uintptr_t flash_start = (uintptr_t)__cy_ml_model_data_load_start__;
    uintptr_t flash_end = (uintptr_t)__cy_ml_model_data_load_end__;
    uint32_t runtime_word0;
    uint32_t runtime_word1;
    uint32_t flash_word0;
    uint32_t flash_word1;
    int blob_same;
    size_t bytes;

    if ((runtime_end <= runtime_start) || (flash_end <= flash_start)) {
        return;
    }

    bytes = (size_t)(runtime_end - runtime_start);
    if (bytes < (2U * sizeof(uint32_t))) {
        return;
    }

    if (runtime_start == flash_start) {
        return;
    }

    memcpy(&runtime_word0, runtime, sizeof(runtime_word0));
    memcpy(&runtime_word1, runtime + sizeof(uint32_t), sizeof(runtime_word1));
    memcpy(&flash_word0, flash, sizeof(flash_word0));
    memcpy(&flash_word1, flash + sizeof(uint32_t), sizeof(flash_word1));
    blob_same = (memcmp(runtime, flash, bytes) == 0);

    /*
     * On this project, startup copy of .cy_ml_model_data can be missed in some boot
     * paths. Refresh runtime model blob before IMAI_init to avoid unaligned parser faults.
     */
    if (!blob_same ||
        (runtime_word0 != flash_word0) || (runtime_word1 != flash_word1) ||
        (runtime_word0 != UVC_AI_MODEL_HEADER_WORD0) || (runtime_word1 != UVC_AI_MODEL_HEADER_WORD1)) {
        memcpy(runtime, flash, bytes);
#if defined(__DCACHE_PRESENT) && (__DCACHE_PRESENT == 1U)
        if ((SCB->CCR & SCB_CCR_DC_Msk) != 0U) {
            SCB_CleanDCache_by_Addr((void *)runtime, (int32_t)bytes);
        }
#endif
        USB_LOG_INFO("blob sync flash->runtime=1 r=0x%08lx f=0x%08lx\r\n",
                     (unsigned long)runtime_start, (unsigned long)flash_start);
    } else {
        USB_LOG_INFO("blob sync flash->runtime=0 r=0x%08lx f=0x%08lx\r\n",
                     (unsigned long)runtime_start, (unsigned long)flash_start);
    }
}

static UVC_AI_ITCM_SECTION void uvc_ai_yuyv_to_rgb888_320x320_u8(const uint8_t *restrict yuyv,
                                                                 uint8_t full_range)
{
    uint32_t y;
    uint8_t *restrict dst = g_model_input_u8;
    const int32_t *restrict y_lut = full_range ? g_uvc_ai_y_lut_full : g_uvc_ai_y_lut;
    const int32_t *restrict u_to_b_lut = full_range ? g_uvc_ai_u_to_b_lut_full : g_uvc_ai_u_to_b_lut;
    const int32_t *restrict u_to_g_lut = full_range ? g_uvc_ai_u_to_g_lut_full : g_uvc_ai_u_to_g_lut;
    const int32_t *restrict v_to_r_lut = full_range ? g_uvc_ai_v_to_r_lut_full : g_uvc_ai_v_to_r_lut;
    const int32_t *restrict v_to_g_lut = full_range ? g_uvc_ai_v_to_g_lut_full : g_uvc_ai_v_to_g_lut;

    if ((dst == RT_NULL) || (yuyv == RT_NULL)) {
        return;
    }

    for (y = 0U; y < UVC_AI_CAMERA_HEIGHT; y++) {
        const uint8_t *src = yuyv + y * UVC_AI_SRC_STRIDE_BYTES;
        uint32_t pair;

        for (pair = 0U; pair < UVC_AI_SRC_PAIR_COUNT; pair++) {
            uint32_t packed = uvc_ai_load_u32_unaligned(src);
            uint8_t y0 = (uint8_t)(packed & 0xffU);
            uint8_t u = (uint8_t)((packed >> 8) & 0xffU);
            uint8_t y1 = (uint8_t)((packed >> 16) & 0xffU);
            uint8_t v = (uint8_t)((packed >> 24) & 0xffU);
            int32_t y_term0 = y_lut[y0];
            int32_t y_term1 = y_lut[y1];
            int32_t u_to_b = u_to_b_lut[u];
            int32_t u_to_g = u_to_g_lut[u];
            int32_t v_to_r = v_to_r_lut[v];
            int32_t v_to_g = v_to_g_lut[v];
            int32_t uv_to_g = u_to_g + v_to_g;

            *dst++ = uvc_ai_sat_to_u8((y_term0 + v_to_r + 128) >> 8);
            *dst++ = uvc_ai_sat_to_u8((y_term0 + uv_to_g + 128) >> 8);
            *dst++ = uvc_ai_sat_to_u8((y_term0 + u_to_b + 128) >> 8);

            *dst++ = uvc_ai_sat_to_u8((y_term1 + v_to_r + 128) >> 8);
            *dst++ = uvc_ai_sat_to_u8((y_term1 + uv_to_g + 128) >> 8);
            *dst++ = uvc_ai_sat_to_u8((y_term1 + u_to_b + 128) >> 8);

            src += 4;
        }
    }
}

static uint8_t uvc_ai_get_best_class(const float *output,
                                     uint32_t score_count,
                                     uint32_t max_predictions_model,
                                     uint32_t det_index,
                                     float *best_score)
{
    uint32_t cls;
    uint8_t best_index = 0U;
    float max_value;

    if ((output == RT_NULL) || (score_count == 0U)) {
        *best_score = 0.0f;
        return 0U;
    }

    max_value = output[(4U * max_predictions_model) + det_index];
    for (cls = 1U; cls < score_count; cls++) {
        float score = output[((4U + cls) * max_predictions_model) + det_index];

        if (score > max_value) {
            max_value = score;
            best_index = (uint8_t)cls;
        }
    }

    *best_score = max_value;
    return best_index;
}

static const char *uvc_ai_risk_label(float conf)
{
    if (conf >= 0.75f) return "HIGH RISK";
    if (conf >= 0.50f) return "MID RISK";
    return "LOW RISK";
}

void uvc_ai_set_mode(uvc_ai_mode_t mode)
{
    g_ai_active_mode = (mode == UVC_AI_MODE_STEEL_BALL) ? UVC_AI_MODE_STEEL_BALL : UVC_AI_MODE_FATIGUE;
}

uvc_ai_mode_t uvc_ai_get_mode(void)
{
    return g_ai_active_mode;
}

uvc_ai_mode_t uvc_ai_get_init_mode(void)
{
    return g_ai_init_mode;
}

uint32_t uvc_ai_get_npu_timeout_count(void)
{
    return mtb_ml_ethosu_get_timeout_count();
}

void uvc_ai_set_conf_threshold(float thresh)
{
    if (!isfinite(thresh)) {
        return;
    }
    if (thresh < 0.15f) thresh = 0.15f;
    if (thresh > 0.95f) thresh = 0.95f;
    g_steel_ball_conf_thresh = thresh;
}

float uvc_ai_get_conf_threshold(void)
{
    return g_steel_ball_conf_thresh;
}

void uvc_ai_invalidate_steel_ball_cache(void)
{
    g_sb_hyperram_loaded = 0U;
    g_sb_loaded_mount_gen = 0U;
    g_sb_model_bin_size = 0U;
}

static int uvc_ai_steel_ball_init(void)
{
    uint32_t arena_size = 0U;
    uint8_t *arena = ml_shared_get_arena(&arena_size);
    cy_rslt_t rslt;
    int i;
    int j;
    int scale_count = 0;
    int found_mask[3] = {0, 0, 0};
    uint16_t in_w;
    uint16_t in_h;
    uint32_t cur_gen = sdcard_get_mount_gen();

    if (g_sb_hyperram_loaded && sdcard_is_mounted() && (cur_gen != g_sb_loaded_mount_gen)) {
        g_sb_hyperram_loaded = 0U;
    }

    if (!g_sb_hyperram_loaded ||
        (g_sb_model_bin_size < 1024U) ||
        (memcmp(g_steel_ball_hyperram_bin + 4U, "TFL3", 4U) != 0)) {
        size_t loaded_sz = 0U;
        int rd;

        if (!sdcard_is_present()) {
            USB_LOG_ERR("SteelBall model requires TF card (%s)\r\n", SD_STEEL_BALL_MODEL_PATH);
            return -RT_ENOENT;
        }
        if (!sdcard_wait_mounted(1500)) {
            USB_LOG_ERR("SteelBall model TF card mount timeout (%s)\r\n", SD_STEEL_BALL_MODEL_PATH);
            return -RT_ENOENT;
        }
        rd = sd_asset_read_file(SD_STEEL_BALL_MODEL_PATH,
                                g_steel_ball_hyperram_bin,
                                STEEL_BALL_MODEL_MAX_BIN_SIZE,
                                &loaded_sz,
                                RT_FALSE);
        if ((rd != RT_EOK) ||
            (loaded_sz < 1024U) ||
            (memcmp(g_steel_ball_hyperram_bin + 4U, "TFL3", 4U) != 0)) {
            USB_LOG_ERR("SteelBall load failed from %s (ret=%d sz=%lu)\r\n",
                        SD_STEEL_BALL_MODEL_PATH, rd, (unsigned long)loaded_sz);
            g_sb_hyperram_loaded = 0U;
            g_sb_loaded_mount_gen = 0U;
            g_sb_model_bin_size = 0U;
            return -RT_EIO;
        }
        g_sb_model_bin_size = (uint32_t)loaded_sz;
        g_sb_loaded_mount_gen = sdcard_get_mount_gen();
        g_sb_hyperram_loaded = 1U;
        USB_LOG_INFO("SteelBall model loaded from TF card (%lu B)\r\n",
                     (unsigned long)g_sb_model_bin_size);
    }

    if ((arena == RT_NULL) || (arena_size < STEEL_BALL_ARENA_SIZE)) {
        USB_LOG_ERR("SteelBall arena too small: %lu\r\n", (unsigned long)arena_size);
        return -RT_ENOMEM;
    }

    {
        mtb_ml_model_bin_t bin = {
            .name = "steel_ball_yolov5nu_320_int8",
            .model_bin = g_steel_ball_hyperram_bin,
            .model_size = g_sb_model_bin_size,
            .arena_size = (int)arena_size,
        };
        mtb_ml_model_buffer_t buf = {
            .tensor_arena = arena,
            .tensor_arena_size = arena_size,
        };

        rslt = mtb_ml_model_init(&bin, &buf, &g_steel_ball_model);
    }
    if (rslt != CY_RSLT_SUCCESS || g_steel_ball_model == RT_NULL) {
        USB_LOG_ERR("SteelBall model init failed: 0x%08lx\r\n", (unsigned long)rslt);
        g_steel_ball_model = RT_NULL;
        return -RT_ERROR;
    }

    rslt = mtb_ml_init(3);
    if (rslt != CY_RSLT_SUCCESS) {
        USB_LOG_ERR("SteelBall mtb_ml_init failed: 0x%08lx\r\n", (unsigned long)rslt);
        mtb_ml_model_deinit(g_steel_ball_model);
        g_steel_ball_model = RT_NULL;
        return -RT_ERROR;
    }

    if ((g_steel_ball_model->input == RT_NULL) ||
        ((g_steel_ball_model->input_size != (int)(320U * 320U * 3U)) &&
         (g_steel_ball_model->input_size != (int)(320U * 240U * 3U))) ||
        (g_steel_ball_model->output_count != 6)) {
        USB_LOG_ERR("SteelBall bad io meta: in=%d out_cnt=%d\r\n",
                    g_steel_ball_model->input_size, g_steel_ball_model->output_count);
        mtb_ml_model_deinit(g_steel_ball_model);
        g_steel_ball_model = RT_NULL;
        mtb_ml_deinit();
        return -RT_ERROR;
    }

    in_w = UVC_AI_IMAGE_WIDTH;
    in_h = (g_steel_ball_model->input_size == (int)(320U * 240U * 3U))
           ? UVC_AI_CAMERA_HEIGHT : UVC_AI_IMAGE_HEIGHT;
    if ((g_steel_ball_model->input_count > 0) &&
        (g_steel_ball_model->inputs != RT_NULL) &&
        (g_steel_ball_model->inputs[0].dim_len == 4) &&
        (g_steel_ball_model->inputs[0].dims != RT_NULL)) {
        int dh = g_steel_ball_model->inputs[0].dims[1];
        int dw = g_steel_ball_model->inputs[0].dims[2];
        int dc = g_steel_ball_model->inputs[0].dims[3];
        if ((dw == (int)UVC_AI_CAMERA_WIDTH) &&
            ((dh == (int)UVC_AI_IMAGE_HEIGHT) || (dh == (int)UVC_AI_CAMERA_HEIGHT)) &&
            (dc == 3)) {
            in_h = (uint16_t)dh;
            in_w = (uint16_t)dw;
        }
    }
    g_sb_input_w = in_w;
    g_sb_input_h = in_h;
    g_sb_input_bytes = (uint32_t)in_w * (uint32_t)in_h * 3U;

    memset(g_sb_scales, 0, sizeof(g_sb_scales));
    for (i = 0; i < g_steel_ball_model->output_count; i++) {
        const mtb_ml_tensor_desc_t *td = &g_steel_ball_model->outputs[i];
        int gh;
        int gw;
        int ch;
        int scale_idx = -1;
        int k;

        if ((td->dim_len != 4) || (td->dims == RT_NULL)) {
            continue;
        }
        gh = td->dims[1];
        gw = td->dims[2];
        ch = td->dims[3];
        if ((gh <= 0) || (gw <= 0) || (gh > (int)in_h) || (gw > (int)in_w)) {
            continue;
        }
        if ((ch != (int)(4U * STEEL_BALL_DFL_REG_MAX)) && (ch != (int)STEEL_BALL_NUM_CLASSES)) {
            continue;
        }

        for (k = 0; k < scale_count; k++) {
            if ((g_sb_scales[k].grid_h == (uint16_t)gh) &&
                (g_sb_scales[k].grid_w == (uint16_t)gw)) {
                scale_idx = k;
                break;
            }
        }
        if (scale_idx < 0) {
            if (scale_count >= 3) {
                continue;
            }
            scale_idx = scale_count++;
            g_sb_scales[scale_idx].grid_h = (uint16_t)gh;
            g_sb_scales[scale_idx].grid_w = (uint16_t)gw;
            g_sb_scales[scale_idx].stride_x = (float)in_w / (float)gw;
            g_sb_scales[scale_idx].stride_y = (float)in_h / (float)gh;
        }

        if (ch == (int)(4U * STEEL_BALL_DFL_REG_MAX)) {
            g_sb_scales[scale_idx].box_ptr = (int8_t *)td->ptr;
            g_sb_scales[scale_idx].box_zp = td->zero_point;
            g_sb_scales[scale_idx].box_scale = td->scale;
            found_mask[scale_idx] |= 1;
        } else if (ch == (int)STEEL_BALL_NUM_CLASSES) {
            g_sb_scales[scale_idx].cls_ptr = (int8_t *)td->ptr;
            g_sb_scales[scale_idx].cls_zp = td->zero_point;
            g_sb_scales[scale_idx].cls_scale = td->scale;
            found_mask[scale_idx] |= 2;
        }
    }

    if ((scale_count != 3) || (found_mask[0] != 3) || (found_mask[1] != 3) || (found_mask[2] != 3)) {
        USB_LOG_ERR("SteelBall output tensors mismatch: cnt=%d mask=%d %d %d\r\n",
                    scale_count, found_mask[0], found_mask[1], found_mask[2]);
        mtb_ml_model_deinit(g_steel_ball_model);
        g_steel_ball_model = RT_NULL;
        mtb_ml_deinit();
        return -RT_ERROR;
    }

    /* Sort scales fine-to-coarse (largest grid_h * grid_w first). */
    for (i = 0; i < 2; i++) {
        for (j = i + 1; j < 3; j++) {
            uint32_t cells_i = (uint32_t)g_sb_scales[i].grid_h * (uint32_t)g_sb_scales[i].grid_w;
            uint32_t cells_j = (uint32_t)g_sb_scales[j].grid_h * (uint32_t)g_sb_scales[j].grid_w;
            if (cells_j > cells_i) {
                steel_ball_scale_desc_t tmp = g_sb_scales[i];
                g_sb_scales[i] = g_sb_scales[j];
                g_sb_scales[j] = tmp;
            }
        }
    }

    for (i = 0; i < 3; i++) {
        steel_ball_scale_desc_t *sc = &g_sb_scales[i];
        float f_min;
        float f_max;
        float max_prob;
        uint32_t d;

        if ((sc->box_ptr == RT_NULL) || (sc->cls_ptr == RT_NULL) ||
            (sc->box_scale <= 0.0f) || (sc->cls_scale <= 0.0f)) {
            USB_LOG_ERR("SteelBall invalid scale %d params\r\n", i);
            mtb_ml_model_deinit(g_steel_ball_model);
            g_steel_ball_model = RT_NULL;
            mtb_ml_deinit();
            return -RT_ERROR;
        }

        f_min = (-128.0f - (float)sc->cls_zp) * sc->cls_scale;
        f_max = (127.0f - (float)sc->cls_zp) * sc->cls_scale;
        sc->cls_is_prob = ((f_min >= -0.05f) && (f_max <= 1.05f)) ? 1U : 0U;
        if (sc->cls_is_prob) {
            max_prob = fminf(1.0f, fmaxf(0.0f, f_max));
        } else {
            max_prob = 1.0f / (1.0f + expf(-f_max));
        }

        for (d = 0U; d < 256U; d++) {
            g_sb_dfl_exp_lut[i][d] = expf(-(float)d * sc->box_scale);
        }

        USB_LOG_INFO("SB head s%d %ux%u prob=%u scale=%.5f zp=%ld max=%.1f%%\r\n",
                     i, (unsigned)sc->grid_h, (unsigned)sc->grid_w,
                     (unsigned)sc->cls_is_prob, sc->cls_scale,
                     (long)sc->cls_zp, max_prob * 100.0f);
    }

    g_sb_fast_quant_input = (uint8_t)((g_steel_ball_model->input_zero_point == -128) &&
                                      (fabsf(g_steel_ball_model->input_scale - (1.0f / 255.0f)) < 1e-5f));
#if defined(__DCACHE_PRESENT) && (__DCACHE_PRESENT == 1U)
    if ((SCB->CCR & SCB_CCR_DC_Msk) != 0U) {
        SCB_CleanInvalidateDCache();
    }
#endif

    USB_LOG_INFO("SteelBall model initialized (%ux%u, arena used=%d/%lu B, fast_in=%u)\r\n",
                 (unsigned)g_sb_input_w, (unsigned)g_sb_input_h,
                 g_steel_ball_model->buffer_size, (unsigned long)arena_size,
                 (unsigned)g_sb_fast_quant_input);
    return RT_EOK;
}

static void uvc_ai_steel_ball_deinit(void)
{
    if (g_steel_ball_model != RT_NULL) {
        mtb_ml_model_deinit(g_steel_ball_model);
        g_steel_ball_model = RT_NULL;
        mtb_ml_deinit();
    }
    memset(g_sb_scales, 0, sizeof(g_sb_scales));
    g_sb_input_w = UVC_AI_IMAGE_WIDTH;
    g_sb_input_h = UVC_AI_IMAGE_HEIGHT;
    g_sb_input_bytes = IMAI_DATA_IN_COUNT;
}

static UVC_AI_ITCM_SECTION void uvc_ai_quantize_u8_to_s8(const uint8_t *restrict src,
                                                         int8_t *restrict dst,
                                                         uint32_t count,
                                                         float in_scale,
                                                         int32_t in_zp)
{
    uint32_t i;

    if ((in_zp == -128) && (fabsf(in_scale - (1.0f / 255.0f)) < 1e-5f)) {
        for (i = 0U; i < count; i++) {
            dst[i] = (int8_t)((int32_t)src[i] - 128);
        }
        return;
    }

    {
        float inv_scale = 1.0f / (255.0f * in_scale);
        for (i = 0U; i < count; i++) {
            int32_t q = (int32_t)lroundf((float)src[i] * inv_scale) + in_zp;
            if (q > 127) q = 127;
            if (q < -128) q = -128;
            dst[i] = (int8_t)q;
        }
    }
}

typedef struct {
    float x1;
    float y1;
    float x2;
    float y2;
    float score;
} sb_cand_t;

static UVC_AI_ITCM_SECTION void uvc_ai_yuyv_to_s8_320x240(const uint8_t *restrict yuyv,
                                                           int8_t *restrict dst,
                                                           uint8_t full_range)
{
    uint32_t y;
    const int32_t *restrict y_lut = full_range ? g_uvc_ai_y_lut_full : g_uvc_ai_y_lut;
    const int32_t *restrict u_to_b_lut = full_range ? g_uvc_ai_u_to_b_lut_full : g_uvc_ai_u_to_b_lut;
    const int32_t *restrict u_to_g_lut = full_range ? g_uvc_ai_u_to_g_lut_full : g_uvc_ai_u_to_g_lut;
    const int32_t *restrict v_to_r_lut = full_range ? g_uvc_ai_v_to_r_lut_full : g_uvc_ai_v_to_r_lut;
    const int32_t *restrict v_to_g_lut = full_range ? g_uvc_ai_v_to_g_lut_full : g_uvc_ai_v_to_g_lut;

    if ((dst == RT_NULL) || (yuyv == RT_NULL)) {
        return;
    }

    for (y = 0U; y < UVC_AI_CAMERA_HEIGHT; y++) {
        const uint8_t *src = yuyv + y * UVC_AI_SRC_STRIDE_BYTES;
        uint32_t pair;

        for (pair = 0U; pair < UVC_AI_SRC_PAIR_COUNT; pair++) {
            uint32_t packed = uvc_ai_load_u32_unaligned(src);
            uint8_t y0 = (uint8_t)(packed & 0xffU);
            uint8_t u = (uint8_t)((packed >> 8) & 0xffU);
            uint8_t y1 = (uint8_t)((packed >> 16) & 0xffU);
            uint8_t v = (uint8_t)((packed >> 24) & 0xffU);
            int32_t y_term0 = y_lut[y0];
            int32_t y_term1 = y_lut[y1];
            int32_t u_to_b = u_to_b_lut[u];
            int32_t u_to_g = u_to_g_lut[u];
            int32_t v_to_r = v_to_r_lut[v];
            int32_t v_to_g = v_to_g_lut[v];
            int32_t uv_to_g = u_to_g + v_to_g;

            *dst++ = (int8_t)((int32_t)uvc_ai_sat_to_u8((y_term0 + v_to_r + 128) >> 8) - 128);
            *dst++ = (int8_t)((int32_t)uvc_ai_sat_to_u8((y_term0 + uv_to_g + 128) >> 8) - 128);
            *dst++ = (int8_t)((int32_t)uvc_ai_sat_to_u8((y_term0 + u_to_b + 128) >> 8) - 128);

            *dst++ = (int8_t)((int32_t)uvc_ai_sat_to_u8((y_term1 + v_to_r + 128) >> 8) - 128);
            *dst++ = (int8_t)((int32_t)uvc_ai_sat_to_u8((y_term1 + uv_to_g + 128) >> 8) - 128);
            *dst++ = (int8_t)((int32_t)uvc_ai_sat_to_u8((y_term1 + u_to_b + 128) >> 8) - 128);

            src += 4;
        }
    }
}

static uint8_t sb_verify_metallic_ball_yuyv(const uint8_t *yuyv,
                                            float x1, float y1,
                                            float x2, float y2)
{
    float bw;
    float bh;
    float min_side;
    float max_side;
    float cx;
    float cy;
    float r;
    float r_patch;
    float r_core2;
    float r_ring_in2;
    float r_ring_out2;
    int32_t px0;
    int32_t py0;
    int32_t px1;
    int32_t py1;
    int32_t step;
    int32_t py;
    uint16_t hist[32];
    uint32_t core_count = 0U;
    uint8_t y_max = 0U;
    uint32_t edge_count = 0U;
    float cos_sum = 0.0f;
    uint8_t octant_mask = 0U;
    uint32_t active_octants = 0U;
    uint32_t target_p15;
    uint32_t target_med;
    uint32_t acc = 0U;
    int32_t y_p15 = -1;
    int32_t y_med = -1;
    uint32_t b;

    if ((yuyv == RT_NULL) || !isfinite(x1) || !isfinite(y1) || !isfinite(x2) || !isfinite(y2)) {
        return 0U;
    }

    bw = x2 - x1;
    bh = y2 - y1;
    if ((bw < 6.0f) || (bh < 6.0f)) {
        return 0U;
    }

    min_side = (bw < bh) ? bw : bh;
    max_side = (bw > bh) ? bw : bh;
    if ((min_side / max_side) < 0.72f) {
        return 0U;
    }

    cx = 0.5f * (x1 + x2);
    cy = 0.5f * (y1 + y2);
    r = 0.25f * (bw + bh);
    r_patch = 1.35f * r;
    r_core2 = (0.95f * r) * (0.95f * r);
    r_ring_in2 = (0.70f * r) * (0.70f * r);
    r_ring_out2 = (1.20f * r) * (1.20f * r);

    px0 = (int32_t)floorf(cx - r_patch);
    py0 = (int32_t)floorf(cy - r_patch);
    px1 = (int32_t)ceilf(cx + r_patch);
    py1 = (int32_t)ceilf(cy + r_patch);

    if (px0 < 1) px0 = 1;
    if (py0 < 1) py0 = 1;
    if (px1 > (int32_t)UVC_AI_CAMERA_WIDTH - 2) px1 = (int32_t)UVC_AI_CAMERA_WIDTH - 2;
    if (py1 > (int32_t)UVC_AI_CAMERA_HEIGHT - 2) py1 = (int32_t)UVC_AI_CAMERA_HEIGHT - 2;
    if ((px1 < px0) || (py1 < py0)) {
        return 0U;
    }

    step = (r > 48.0f) ? 3 : ((r > 24.0f) ? 2 : 1);
    memset(hist, 0, sizeof(hist));

    for (py = py0; py <= py1; py += step) {
        const uint8_t *row_m = yuyv + (uint32_t)(py - 1) * UVC_AI_SRC_STRIDE_BYTES;
        const uint8_t *row_c = yuyv + (uint32_t)py * UVC_AI_SRC_STRIDE_BYTES;
        const uint8_t *row_p = yuyv + (uint32_t)(py + 1) * UVC_AI_SRC_STRIDE_BYTES;
        float dy = (float)py - cy;
        float dy2 = dy * dy;
        int32_t px;

        for (px = px0; px <= px1; px += step) {
            float dx = (float)px - cx;
            float dist2 = dx * dx + dy2;

            if (dist2 <= r_core2) {
                uint8_t yv = row_c[(uint32_t)px << 1];
                if (yv > y_max) {
                    y_max = yv;
                }
                hist[yv >> 3]++;
                core_count++;
            }

            if ((dist2 >= r_ring_in2) && (dist2 <= r_ring_out2)) {
                uint32_t xm = (uint32_t)(px - 1) << 1;
                uint32_t xc = (uint32_t)px << 1;
                uint32_t xp = (uint32_t)(px + 1) << 1;
                int16_t tl = (int16_t)row_m[xm];
                int16_t tc = (int16_t)row_m[xc];
                int16_t tr = (int16_t)row_m[xp];
                int16_t ml = (int16_t)row_c[xm];
                int16_t mr = (int16_t)row_c[xp];
                int16_t bl = (int16_t)row_p[xm];
                int16_t bc = (int16_t)row_p[xc];
                int16_t br = (int16_t)row_p[xp];
                int16_t gx = (int16_t)((tr + (2 * mr) + br) - (tl + (2 * ml) + bl));
                int16_t gy = (int16_t)((bl + (2 * bc) + br) - (tl + (2 * tc) + tr));
                int32_t g2 = (int32_t)gx * (int32_t)gx + (int32_t)gy * (int32_t)gy;

                if (g2 >= 900) {
                    float dot = fabsf((float)gx * dx + (float)gy * dy);
                    float denom = sqrtf((float)g2 * dist2);
                    float cos_abs = (denom > 1e-4f) ? (dot / denom) : 0.0f;

                    cos_sum += cos_abs;
                    edge_count++;
                    if (cos_abs >= 0.50f) {
                        uint8_t oct = (uint8_t)(((dx < 0.0f) ? 4U : 0U) |
                                                ((dy < 0.0f) ? 2U : 0U) |
                                                ((fabsf(dy) > fabsf(dx)) ? 1U : 0U));
                        octant_mask |= (uint8_t)(1U << oct);
                    }
                }
            }
        }
    }

    if ((core_count < 8U) || (edge_count < 8U)) {
        return 0U;
    }

    for (b = 0U; b < 8U; b++) {
        if ((octant_mask & (1U << b)) != 0U) {
            active_octants++;
        }
    }

    if (((cos_sum / (float)edge_count) < 0.72f) || (active_octants < 5U)) {
        return 0U;
    }

    target_p15 = (core_count * 15U) / 100U;
    target_med = core_count / 2U;
    for (b = 0U; b < 32U; b++) {
        uint32_t prev_acc = acc;
        uint32_t cnt = (uint32_t)hist[b];
        acc += cnt;
        if ((y_p15 < 0) && (acc > target_p15)) {
            y_p15 = (int32_t)(b * 8U) + (int32_t)(((target_p15 - prev_acc) * 8U) / cnt);
        }
        if ((y_med < 0) && (acc > target_med)) {
            y_med = (int32_t)(b * 8U) + (int32_t)(((target_med - prev_acc) * 8U) / cnt);
            break;
        }
    }

    if ((y_p15 < 0) || (y_med < 0)) {
        return 0U;
    }

    if ((((int32_t)y_max - y_p15) < 75) || (((int32_t)y_max - y_med) < 40)) {
        return 0U;
    }

    return 1U;
}

static void sb_emit_detection(uvc_ai_result_t *dst,
                              float x1, float y1, float x2, float y2,
                              float score)
{
    uint32_t out_idx;

    if ((dst == RT_NULL) || (dst->count >= UVC_AI_MAX_PREDICTIONS)) {
        return;
    }
    out_idx = dst->count;
    (void)rt_snprintf(dst->class_string[out_idx], UVC_AI_MAX_CLASS_LEN, "SteelBall");
    dst->class_id[out_idx] = 0U;
    dst->conf[out_idx] = score;
    dst->bbox_int16[out_idx * 4U + 0U] = uvc_ai_clamp_i16((int32_t)lroundf(x1), 0, (int32_t)UVC_AI_CAMERA_WIDTH - 1);
    dst->bbox_int16[out_idx * 4U + 1U] = uvc_ai_clamp_i16((int32_t)lroundf(y1), 0, (int32_t)UVC_AI_CAMERA_HEIGHT - 1);
    dst->bbox_int16[out_idx * 4U + 2U] = uvc_ai_clamp_i16((int32_t)lroundf(x2), 0, (int32_t)UVC_AI_CAMERA_WIDTH - 1);
    dst->bbox_int16[out_idx * 4U + 3U] = uvc_ai_clamp_i16((int32_t)lroundf(y2), 0, (int32_t)UVC_AI_CAMERA_HEIGHT - 1);
    dst->count = (uint8_t)(out_idx + 1U);
}

static void sb_sort_cands_desc(sb_cand_t *cands, uint32_t count)
{
    static const uint16_t gaps[] = {132U, 57U, 23U, 10U, 4U, 1U};
    uint32_t g_idx;

    for (g_idx = 0U; g_idx < (sizeof(gaps) / sizeof(gaps[0])); g_idx++) {
        uint32_t gap = gaps[g_idx];
        uint32_t i;

        for (i = gap; i < count; i++) {
            sb_cand_t tmp = cands[i];
            uint32_t j = i;

            while ((j >= gap) && (cands[j - gap].score < tmp.score)) {
                cands[j] = cands[j - gap];
                j -= gap;
            }
            cands[j] = tmp;
        }
    }
}

static int uvc_ai_steel_ball_compute(const uint8_t *yuyv,
                                     uvc_ai_result_t *high_result,
                                     uvc_ai_result_t *low_result)
{
    int8_t *in_buf = (int8_t *)g_steel_ball_model->input;
    uint8_t *scratch = g_sb_decode_scratch;
    cy_rslt_t rslt;
    int s_idx;
    sb_cand_t *cands;
    uint8_t *suppressed;
    uint32_t cand_count = 0U;
    uint32_t min_idx = 0U;
    uint32_t verify_calls = 0U;
    float min_score = 2.0f;
    float t_high = g_steel_ball_conf_thresh;
    float t_low;
    float t_rescue;
    float logit_thresh;
    uint32_t i;
    uint32_t j;

    if (sizeof(g_sb_decode_scratch) < (SB_MAX_CANDIDATES * sizeof(sb_cand_t) + SB_MAX_CANDIDATES)) {
        return -RT_ENOMEM;
    }
    cands = (sb_cand_t *)scratch;
    suppressed = scratch + SB_MAX_CANDIDATES * sizeof(sb_cand_t);

    if (g_sb_fast_quant_input) {
        /* The tensor arena can reuse input storage during inference. Fill the
         * letterbox padding band (Ultralytics gray 114 -> int8 -14 = 0xF2) on 320x320 models. */
        if (g_sb_input_h > UVC_AI_CAMERA_HEIGHT) {
            memset(in_buf + UVC_AI_TOP_RGB888_BYTES, UVC_AI_SB_PAD_VALUE_S8, UVC_AI_PAD_RGB888_BYTES);
        }
    } else {
        uvc_ai_quantize_u8_to_s8(g_model_input_u8,
                                 in_buf,
                                 g_sb_input_bytes,
                                 g_steel_ball_model->input_scale,
                                 g_steel_ball_model->input_zero_point);
    }

#if defined(__DCACHE_PRESENT) && (__DCACHE_PRESENT == 1U)
    if ((mtb_ml_get_cache_mgmt_type() != MTB_ML_ETHOSU_CACHE_MGMT_OUTER_LAYERS) &&
        ((SCB->CCR & SCB_CCR_DC_Msk) != 0U) &&
        (in_buf != RT_NULL) && (g_sb_input_bytes > 0U)) {
        uintptr_t p = (uintptr_t)in_buf;
        uintptr_t aligned_start = p & ~(uintptr_t)31U;
        uintptr_t aligned_end = (p + g_sb_input_bytes + 31U) & ~(uintptr_t)31U;
        SCB_CleanDCache_by_Addr((void *)aligned_start, (int32_t)(aligned_end - aligned_start));
    }
#endif

    rslt = mtb_ml_model_invoke(g_steel_ball_model);
    if (rslt != CY_RSLT_SUCCESS) {
        USB_LOG_ERR("SteelBall invoke failed: 0x%08lx\r\n", (unsigned long)rslt);
        return -RT_ERROR;
    }

#if defined(__DCACHE_PRESENT) && (__DCACHE_PRESENT == 1U)
    if ((mtb_ml_get_cache_mgmt_type() != MTB_ML_ETHOSU_CACHE_MGMT_OUTER_LAYERS) &&
        ((SCB->CCR & SCB_CCR_DC_Msk) != 0U)) {
        int out_k;
        for (out_k = 0; out_k < g_steel_ball_model->output_count; out_k++) {
            const void *optr = g_steel_ball_model->outputs[out_k].ptr;
            size_t b = g_steel_ball_model->outputs[out_k].bytes;
            if ((optr != RT_NULL) && (b > 0U)) {
                uintptr_t p = (uintptr_t)optr;
                uintptr_t aligned_start = p & ~(uintptr_t)31U;
                uintptr_t aligned_end = (p + b + 31U) & ~(uintptr_t)31U;
                SCB_InvalidateDCache_by_Addr((void *)aligned_start, (int32_t)(aligned_end - aligned_start));
            }
        }
    }
#endif

    if (t_high < 0.15f) t_high = 0.15f;
    if (t_high > 0.95f) t_high = 0.95f;
    t_low = fmaxf(0.22f, t_high - 0.30f);
    if (t_low > t_high) t_low = t_high;
    t_rescue = fmaxf(0.32f, t_high - 0.23f);
    logit_thresh = logf(t_low / (1.0f - t_low));

    /* Scan coarse-to-fine (P5 -> P4 -> P3) with cross-scale Top-K min replacement. */
    for (s_idx = 2; s_idx >= 0; s_idx--) {
        const steel_ball_scale_desc_t *sc = &g_sb_scales[s_idx];
        const float *exp_lut = g_sb_dfl_exp_lut[s_idx];
        uint32_t grid_h = sc->grid_h;
        uint32_t grid_w = sc->grid_w;
        float stride_x = sc->stride_x;
        float stride_y = sc->stride_y;
        float raw_f = ((sc->cls_is_prob ? t_low : logit_thresh) / sc->cls_scale) + (float)sc->cls_zp;
        int32_t raw_min = (int32_t)floorf(raw_f);
        uint32_t gy;

        if (raw_min > 127) {
            continue;
        }
        if (raw_min < -128) {
            raw_min = -128;
        }

        for (gy = 0U; gy < grid_h; gy++) {
            uint32_t gx;
            if ((float)gy * stride_y >= (float)UVC_AI_CAMERA_HEIGHT) {
                break;
            }
            for (gx = 0U; gx < grid_w; gx++) {
                uint32_t cell = gy * grid_w + gx;
                int32_t q_cls = (int32_t)sc->cls_ptr[cell];
                float prob;
                const int8_t *box_base;
                float dists[4];
                float x1;
                float y1;
                float x2;
                float y2;
                int side;

                if (q_cls < raw_min) {
                    continue;
                }

                if (sc->cls_is_prob) {
                    prob = ((float)q_cls - (float)sc->cls_zp) * sc->cls_scale;
                    if (prob > 1.0f) {
                        prob = 1.0f;
                    }
                } else {
                    float logit = ((float)q_cls - (float)sc->cls_zp) * sc->cls_scale;
                    prob = 1.0f / (1.0f + expf(-logit));
                }

                if (prob < t_low) {
                    continue;
                }

                if ((cand_count >= SB_MAX_CANDIDATES) && (prob <= min_score)) {
                    continue;
                }

                box_base = sc->box_ptr + (cell * (4U * STEEL_BALL_DFL_REG_MAX));
                for (side = 0; side < 4; side++) {
                    const int8_t *bptr = box_base + (uint32_t)side * STEEL_BALL_DFL_REG_MAX;
                    int32_t q_max = (int32_t)bptr[0];
                    float sum_exp = 0.0f;
                    float weighted = 0.0f;
                    uint32_t k;

                    for (k = 1U; k < STEEL_BALL_DFL_REG_MAX; k++) {
                        int32_t qv = (int32_t)bptr[k];
                        if (qv > q_max) {
                            q_max = qv;
                        }
                    }
                    for (k = 0U; k < STEEL_BALL_DFL_REG_MAX; k++) {
                        float v = exp_lut[(uint32_t)(q_max - (int32_t)bptr[k])];
                        sum_exp += v;
                        weighted += (float)k * v;
                    }
                    dists[side] = (sum_exp > 0.0f) ? (weighted / sum_exp) : 0.0f;
                }

                x1 = fmaxf(0.0f, ((float)gx + 0.5f - dists[0]) * stride_x);
                y1 = fmaxf(0.0f, ((float)gy + 0.5f - dists[1]) * stride_y);
                x2 = fminf((float)UVC_AI_CAMERA_WIDTH - 1.0f,
                           ((float)gx + 0.5f + dists[2]) * stride_x);
                y2 = fminf((float)UVC_AI_CAMERA_HEIGHT - 1.0f,
                           ((float)gy + 0.5f + dists[3]) * stride_y);
                if (!isfinite(x1) || !isfinite(y1) || !isfinite(x2) || !isfinite(y2) ||
                    !isfinite(prob) || (lroundf(x2) <= lroundf(x1)) || (lroundf(y2) <= lroundf(y1))) {
                    continue;
                }

                if (cand_count < SB_MAX_CANDIDATES) {
                    cands[cand_count].x1 = x1;
                    cands[cand_count].y1 = y1;
                    cands[cand_count].x2 = x2;
                    cands[cand_count].y2 = y2;
                    cands[cand_count].score = prob;
                    if (prob < min_score) {
                        min_score = prob;
                        min_idx = cand_count;
                    }
                    cand_count++;
                } else {
                    uint32_t k;
                    cands[min_idx].x1 = x1;
                    cands[min_idx].y1 = y1;
                    cands[min_idx].x2 = x2;
                    cands[min_idx].y2 = y2;
                    cands[min_idx].score = prob;
                    min_score = cands[0].score;
                    min_idx = 0U;
                    for (k = 1U; k < SB_MAX_CANDIDATES; k++) {
                        if (cands[k].score < min_score) {
                            min_score = cands[k].score;
                            min_idx = k;
                        }
                    }
                }
            }
        }
    }

    /* Sort descending by confidence */
    sb_sort_cands_desc(cands, cand_count);

    /* DIoU-NMS + Lite-WBF + Metallic Ball Physics Verifier */
    memset(suppressed, 0, cand_count);
    for (i = 0U; i < cand_count; i++) {
        float ax1;
        float ay1;
        float ax2;
        float ay2;
        float area_a;
        float acx;
        float acy;
        float wi;
        float w_sum;
        float fx1;
        float fy1;
        float fx2;
        float fy2;
        float out_x1;
        float out_y1;
        float out_x2;
        float out_y2;
        float score;

        if (high_result->count >= UVC_AI_MAX_PREDICTIONS) {
            break;
        }
        if (suppressed[i]) {
            continue;
        }

        score = cands[i].score;
        if ((score < t_high) &&
            ((score < t_rescue) || (verify_calls >= SB_MAX_VERIFY_PER_FRAME)) &&
            ((low_result == RT_NULL) || (low_result->count >= UVC_AI_MAX_PREDICTIONS))) {
            break;
        }

        ax1 = cands[i].x1;
        ay1 = cands[i].y1;
        ax2 = cands[i].x2;
        ay2 = cands[i].y2;
        area_a = (ax2 > ax1 && ay2 > ay1) ? ((ax2 - ax1) * (ay2 - ay1)) : 0.0f;
        acx = 0.5f * (ax1 + ax2);
        acy = 0.5f * (ay1 + ay2);

        wi = score * score;
        w_sum = wi;
        fx1 = ax1 * wi;
        fy1 = ay1 * wi;
        fx2 = ax2 * wi;
        fy2 = ay2 * wi;

        for (j = i + 1U; j < cand_count; j++) {
            float bx1;
            float by1;
            float bx2;
            float by2;
            float area_b;
            float ix1;
            float iy1;
            float ix2;
            float iy2;
            float inter;
            float union_a;
            float iou;
            float cx1;
            float cy1;
            float cx2;
            float cy2;
            float cw;
            float ch;
            float c2;
            float bcx;
            float bcy;
            float dx;
            float dy;
            float rho2;
            float diou;

            if (suppressed[j]) {
                continue;
            }
            bx1 = cands[j].x1;
            by1 = cands[j].y1;
            bx2 = cands[j].x2;
            by2 = cands[j].y2;
            area_b = (bx2 > bx1 && by2 > by1) ? ((bx2 - bx1) * (by2 - by1)) : 0.0f;

            ix1 = (ax1 > bx1) ? ax1 : bx1;
            iy1 = (ay1 > by1) ? ay1 : by1;
            ix2 = (ax2 < bx2) ? ax2 : bx2;
            iy2 = (ay2 < by2) ? ay2 : by2;
            inter = (ix2 > ix1 && iy2 > iy1) ? ((ix2 - ix1) * (iy2 - iy1)) : 0.0f;
            if (inter == 0.0f) {
                continue;
            }
            union_a = area_a + area_b - inter;
            if (union_a <= 0.0f) {
                continue;
            }

            iou = inter / union_a;
            /* DIoU only subtracts a non-negative distance penalty from IoU. */
            if (iou <= 0.45f) {
                continue;
            }
            cx1 = (ax1 < bx1) ? ax1 : bx1;
            cy1 = (ay1 < by1) ? ay1 : by1;
            cx2 = (ax2 > bx2) ? ax2 : bx2;
            cy2 = (ay2 > by2) ? ay2 : by2;
            cw = cx2 - cx1;
            ch = cy2 - cy1;
            c2 = cw * cw + ch * ch;
            bcx = 0.5f * (bx1 + bx2);
            bcy = 0.5f * (by1 + by2);
            dx = acx - bcx;
            dy = acy - bcy;
            rho2 = dx * dx + dy * dy;
            diou = (c2 > 1e-6f) ? (iou - (rho2 / c2)) : iou;

            if (diou > 0.45f) {
                suppressed[j] = 1U;
                if (iou > 0.55f) {
                    float wj = cands[j].score * cands[j].score;
                    w_sum += wj;
                    fx1 += bx1 * wj;
                    fy1 += by1 * wj;
                    fx2 += bx2 * wj;
                    fy2 += by2 * wj;
                }
            }
        }

        if (w_sum > 0.0f) {
            float inv_w = 1.0f / w_sum;
            out_x1 = fx1 * inv_w;
            out_y1 = fy1 * inv_w;
            out_x2 = fx2 * inv_w;
            out_y2 = fy2 * inv_w;
        } else {
            out_x1 = ax1;
            out_y1 = ay1;
            out_x2 = ax2;
            out_y2 = ay2;
        }

        if (score >= t_high) {
            sb_emit_detection(high_result, out_x1, out_y1, out_x2, out_y2, score);
        } else if (score >= t_low) {
            uint8_t verified = 0U;
            if ((score >= t_rescue) &&
                (high_result->count < UVC_AI_MAX_PREDICTIONS) &&
                (verify_calls < SB_MAX_VERIFY_PER_FRAME)) {
                verify_calls++;
                verified = sb_verify_metallic_ball_yuyv(yuyv, out_x1, out_y1, out_x2, out_y2);
            }
            if (verified != 0U) {
                sb_emit_detection(high_result, out_x1, out_y1, out_x2, out_y2, score);
            } else if ((low_result != RT_NULL) && (low_result->count < UVC_AI_MAX_PREDICTIONS)) {
                sb_emit_detection(low_result, out_x1, out_y1, out_x2, out_y2, score);
            }
        }
    }

    high_result->valid = 1U;
    if (low_result != RT_NULL) {
        low_result->valid = 1U;
    }
    return RT_EOK;
}

void uvc_ai_reset_result(uvc_ai_result_t *result)
{
    if (result == RT_NULL) {
        return;
    }

    memset(result, 0, sizeof(*result));
}

int uvc_ai_init(const uvc_ai_config_t *config)
{
    int ret;
    uint8_t pad_u8;

    if (config != RT_NULL) {
        g_src_width = (config->src_width == 0U) ? UVC_AI_CAMERA_WIDTH : config->src_width;
        g_src_height = (config->src_height == 0U) ? UVC_AI_CAMERA_HEIGHT : config->src_height;
    } else {
        g_src_width = UVC_AI_CAMERA_WIDTH;
        g_src_height = UVC_AI_CAMERA_HEIGHT;
    }

    if ((g_src_width != UVC_AI_CAMERA_WIDTH) || (g_src_height != UVC_AI_CAMERA_HEIGHT)) {
        USB_LOG_WRN("AI only supports 320x240 input, force %ux%u\r\n",
                    (unsigned)UVC_AI_CAMERA_WIDTH, (unsigned)UVC_AI_CAMERA_HEIGHT);
        g_src_width = UVC_AI_CAMERA_WIDTH;
        g_src_height = UVC_AI_CAMERA_HEIGHT;
    }

    uvc_ai_prepare_yuv_lut();

    if (g_ai_initialized) {
        return RT_EOK;
    }

    g_ai_init_mode = g_ai_active_mode;

    if ((SCB->CCR & SCB_CCR_UNALIGN_TRP_Msk) != 0U) {
        g_saved_ccr = SCB->CCR;
        SCB->CCR = g_saved_ccr & (~SCB_CCR_UNALIGN_TRP_Msk);
        __DSB();
        __ISB();
        g_restore_unalign_trp = 1U;
        USB_LOG_WRN("UNALIGN_TRP disabled for TFLM parser compatibility\r\n");
    } else {
        g_restore_unalign_trp = 0U;
    }

    if (g_ai_init_mode == UVC_AI_MODE_STEEL_BALL) {
        ret = uvc_ai_steel_ball_init();
        if (ret != RT_EOK) {
            if (g_restore_unalign_trp) {
                SCB->CCR = g_saved_ccr;
                __DSB();
                __ISB();
                g_restore_unalign_trp = 0U;
            }
            return ret;
        }
    } else {
        uvc_ai_sync_model_blob_from_flash();
        ret = IMAI_init();
        if (ret != IMAI_RET_SUCCESS) {
            USB_LOG_ERR("IMAI_init failed: %d\r\n", ret);
            if (g_restore_unalign_trp) {
                SCB->CCR = g_saved_ccr;
                __DSB();
                __ISB();
                g_restore_unalign_trp = 0U;
            }
            return -RT_ERROR;
        }
    }

    /*
     * AI input is fixed to 320x240 top area + 80-line bottom padding.
     * Fatigue keeps 0 padding; Steel Ball uses Ultralytics gray (114).
     */
    pad_u8 = (g_ai_init_mode == UVC_AI_MODE_STEEL_BALL)
             ? UVC_AI_SB_PAD_VALUE_U8 : UVC_AI_PAD_VALUE_U8;
    memset(g_model_input_u8 + UVC_AI_TOP_RGB888_BYTES,
           pad_u8,
           UVC_AI_PAD_RGB888_BYTES);
#if defined(__DCACHE_PRESENT) && (__DCACHE_PRESENT == 1U)
    if ((SCB->CCR & SCB_CCR_DC_Msk) != 0U) {
        SCB_CleanDCache_by_Addr((void *)(g_model_input_u8 + UVC_AI_TOP_RGB888_BYTES),
                                (int32_t)UVC_AI_PAD_RGB888_BYTES);
        SCB_CleanDCache();
    }
#endif

    mtb_ml_set_cache_mgmt_type(MTB_ML_ETHOSU_CACHE_MGMT_OUTER_LAYERS);

    g_last_npu_cycles = 0U;
    g_last_score_count = 0U;
    g_ai_initialized = 1U;
    USB_LOG_INFO("AI clocks cpu=%luMHz npu=%luMHz cache=%lu\r\n",
                 (unsigned long)(mtb_ml_cpu_clk_freq / 1000000U),
                 (unsigned long)(mtb_ml_npu_clk_freq / 1000000U),
                 (unsigned long)mtb_ml_get_cache_mgmt_type());
    USB_LOG_INFO("AI model initialized (%s)\r\n",
                 (g_ai_init_mode == UVC_AI_MODE_STEEL_BALL) ? "steel_ball" : "fatigue");
    return RT_EOK;
}

void uvc_ai_deinit(void)
{
    if (!g_ai_initialized) {
        return;
    }

    if (g_ai_init_mode == UVC_AI_MODE_STEEL_BALL) {
        uvc_ai_steel_ball_deinit();
    } else {
        IMAI_finalize();
    }
    if (g_restore_unalign_trp) {
        SCB->CCR = g_saved_ccr;
        __DSB();
        __ISB();
        g_restore_unalign_trp = 0U;
    }
    g_last_npu_cycles = 0U;
    g_last_score_count = 0U;
    g_ai_initialized = 0U;
    USB_LOG_INFO("AI model deinitialized\r\n");
}

UVC_AI_ITCM_SECTION int uvc_ai_process_yuyv_ex(const uint8_t *yuyv, uint32_t yuyv_size,
                                               uvc_ai_result_t *high_result,
                                               uvc_ai_result_t *low_result)
{
    IMAI_api_def *api_def;
    uint32_t min_frame_size;
    uint32_t max_predictions_model;
    uint32_t max_predictions;
    uint32_t output_columns;
    uint32_t detection_flag_index;
    uint32_t score_count;
    uint32_t i;
    uint64_t npu_cycles_before;
    uint64_t npu_cycles_after;
    uint64_t npu_cycles_delta;
    rt_tick_t prep_start_tick;
    rt_tick_t prep_end_tick;
    rt_tick_t start_tick;
    rt_tick_t end_tick;
    rt_tick_t log_delta_tick;
    rt_tick_t log_now_tick;
    float prep_ms;
    float npu_ms;

    if ((!g_ai_initialized) || (yuyv == RT_NULL) || (high_result == RT_NULL)) {
        return -RT_EINVAL;
    }

    min_frame_size = UVC_AI_FRAME_BYTES;
    if (yuyv_size < min_frame_size) {
        USB_LOG_WRN("AI frame too small: %lu < %lu\r\n",
                    (unsigned long)yuyv_size, (unsigned long)min_frame_size);
        return -RT_EINVAL;
    }

    prep_start_tick = rt_tick_get();
    if (g_ai_init_mode == UVC_AI_MODE_STEEL_BALL) {
        uint8_t full_range = uvc_ai_probe_full_range_yuyv(yuyv);
        if (g_sb_fast_quant_input) {
            uvc_ai_yuyv_to_s8_320x240(yuyv, (int8_t *)g_steel_ball_model->input, full_range);
        } else {
            uvc_ai_yuyv_to_rgb888_320x320_u8(yuyv, full_range);
        }
    } else {
        uvc_ai_yuyv_to_rgb888_320x320_u8(yuyv, 0U);
    }
    prep_end_tick = rt_tick_get();

    uvc_ai_reset_result(high_result);
    if (low_result != RT_NULL) {
        uvc_ai_reset_result(low_result);
    }

    npu_cycles_before = mtb_ml_npu_cycles;
    start_tick = rt_tick_get();
    if (g_ai_init_mode == UVC_AI_MODE_STEEL_BALL) {
        int sb_ret = uvc_ai_steel_ball_compute(yuyv, high_result, low_result);
        end_tick = rt_tick_get();
        npu_cycles_after = mtb_ml_npu_cycles;
        if (sb_ret != RT_EOK) {
            return sb_ret;
        }
        goto post_infer;
    }
    IMAI_compute(g_model_input_u8, g_model_output);
    end_tick = rt_tick_get();
    npu_cycles_after = mtb_ml_npu_cycles;

    api_def = IMAI_api();
    if ((api_def == RT_NULL) || (api_def->func_count <= 0)) {
        USB_LOG_ERR("IMAI_api metadata unavailable\r\n");
        return -RT_ERROR;
    }

    max_predictions_model = (uint32_t)api_def->func_list[0].param_list[1].shape[0].size;
    output_columns = (uint32_t)api_def->func_list[0].param_list[1].shape[1].size;
    if ((max_predictions_model == 0U) || (output_columns < 6U)) {
        USB_LOG_ERR("Invalid model output shape\r\n");
        return -RT_ERROR;
    }

    max_predictions = max_predictions_model;
    if (max_predictions > UVC_AI_MAX_PREDICTIONS) {
        max_predictions = UVC_AI_MAX_PREDICTIONS;
    }

    detection_flag_index = output_columns - 1U;
    score_count = output_columns - 5U;
    if (score_count == 0U) {
        USB_LOG_ERR("Model output has no class scores\r\n");
        return -RT_ERROR;
    }
    if ((score_count != UVC_AI_NUM_CLASSES) && (score_count != g_last_score_count)) {
        USB_LOG_WRN("Model class count=%lu (default=%u), using runtime class metadata\r\n",
                    (unsigned long)score_count, (unsigned)UVC_AI_NUM_CLASSES);
        g_last_score_count = score_count;
    }

    for (i = 0U; i < max_predictions; i++) {
        float detection_flag = g_model_output[detection_flag_index * max_predictions_model + i];
        float x;
        float y;
        float w;
        float h;
        float best_score = 0.0f;
        uint8_t best_class;
        const char *label;
        uint32_t label_idx;
        int32_t xmin;
        int32_t ymin;
        int32_t xmax;
        int32_t ymax;
        uint32_t out_idx;

        if (detection_flag == 0.0f) {
            break;
        }

        out_idx = high_result->count;
        if (out_idx >= UVC_AI_MAX_PREDICTIONS) {
            break;
        }

        x = g_model_output[0U * max_predictions_model + i];
        y = g_model_output[1U * max_predictions_model + i];
        w = g_model_output[2U * max_predictions_model + i];
        h = g_model_output[3U * max_predictions_model + i];

        best_class = uvc_ai_get_best_class(g_model_output,
                                           score_count,
                                           max_predictions_model,
                                           i,
                                           &best_score);

        label = "Unknown";
        label_idx = (uint32_t)best_class + 4U;
        if ((label_idx < detection_flag_index) &&
            (api_def->func_list[0].param_list[1].shape != RT_NULL) &&
            (api_def->func_list[0].param_list[1].shape[1].labels != RT_NULL) &&
            (api_def->func_list[0].param_list[1].shape[1].labels[label_idx] != RT_NULL)) {
            label = api_def->func_list[0].param_list[1].shape[1].labels[label_idx];
        } else if (score_count == 1U) {
            label = "Face";
        }

        if (label == RT_NULL) {
            label = "Unknown";
        }

        (void)rt_snprintf(high_result->class_string[out_idx], UVC_AI_MAX_CLASS_LEN, "%s", label);
        high_result->class_id[out_idx] = best_class;
        high_result->conf[out_idx] = best_score;

        xmin = (int32_t)((x - UVC_AI_HALF(w)) * UVC_AI_IMAGE_WIDTH + UVC_AI_ROUND_FACTOR);
        ymin = (int32_t)((y - UVC_AI_HALF(h)) * UVC_AI_IMAGE_WIDTH + UVC_AI_ROUND_FACTOR);
        xmax = (int32_t)((x + UVC_AI_HALF(w)) * UVC_AI_IMAGE_WIDTH + UVC_AI_ROUND_FACTOR);
        ymax = (int32_t)((y + UVC_AI_HALF(h)) * UVC_AI_IMAGE_WIDTH + UVC_AI_ROUND_FACTOR);

        high_result->bbox_int16[out_idx * 4U + 0U] = uvc_ai_clamp_i16(xmin, 0, (int32_t)UVC_AI_IMAGE_WIDTH - 1);
        high_result->bbox_int16[out_idx * 4U + 1U] = uvc_ai_clamp_i16(ymin, 0, (int32_t)UVC_AI_IMAGE_HEIGHT - 1);
        high_result->bbox_int16[out_idx * 4U + 2U] = uvc_ai_clamp_i16(xmax, 0, (int32_t)UVC_AI_IMAGE_WIDTH - 1);
        high_result->bbox_int16[out_idx * 4U + 3U] = uvc_ai_clamp_i16(ymax, 0, (int32_t)UVC_AI_IMAGE_HEIGHT - 1);
        high_result->count = (uint8_t)(out_idx + 1U);
    }

    high_result->valid = 1U;
post_infer:
    prep_ms = ((float)(prep_end_tick - prep_start_tick) * 1000.0f) / (float)RT_TICK_PER_SECOND;
    high_result->inference_ms = ((float)(end_tick - start_tick) * 1000.0f) / (float)RT_TICK_PER_SECOND;
    if (low_result != RT_NULL) {
        low_result->inference_ms = high_result->inference_ms;
    }

    if (npu_cycles_after >= npu_cycles_before) {
        npu_cycles_delta = npu_cycles_after - npu_cycles_before;
    } else {
        npu_cycles_delta = 0U;
    }

    if (npu_cycles_after >= g_last_npu_cycles) {
        g_last_npu_cycles = npu_cycles_after;
    } else {
        /* PMU counter could reset after re-init/overflow. */
        g_last_npu_cycles = 0U;
    }

    if (mtb_ml_npu_clk_freq != 0U) {
        npu_ms = ((float)npu_cycles_delta * 1000.0f) / (float)mtb_ml_npu_clk_freq;
    } else {
        npu_ms = 0.0f;
    }

    log_now_tick = rt_tick_get();
    log_delta_tick = log_now_tick - g_last_ai_log_tick;
    if ((g_last_ai_log_tick == 0U) || (log_delta_tick >= (3U * RT_TICK_PER_SECOND))) {
        if (high_result->count > 0U) {
            const char *risk = (strncmp(high_result->class_string[0], "Closed", 6U) == 0)
                               ? uvc_ai_risk_label(high_result->conf[0]) : "";
            USB_LOG_INFO("AI %.1fms (prep %.1f npu %.1f) det=%u top0=%s %.1f%%%s%s [%d,%d,%d,%d]\r\n",
                         high_result->inference_ms, prep_ms, npu_ms, high_result->count,
                         high_result->class_string[0],
                         high_result->conf[0] * 100.0f,
                         (risk[0] != '\0') ? " " : "",
                         risk,
                         high_result->bbox_int16[0],
                         high_result->bbox_int16[1],
                         high_result->bbox_int16[2],
                         high_result->bbox_int16[3]);
        } else {
            USB_LOG_INFO("AI %.1fms (prep %.1f npu %.1f) det=0\r\n",
                         high_result->inference_ms, prep_ms, npu_ms);
        }
        g_last_ai_log_tick = log_now_tick;
    }

    return RT_EOK;
}

int uvc_ai_process_yuyv(const uint8_t *yuyv, uint32_t yuyv_size, uvc_ai_result_t *result)
{
    return uvc_ai_process_yuyv_ex(yuyv, yuyv_size, result, RT_NULL);
}
