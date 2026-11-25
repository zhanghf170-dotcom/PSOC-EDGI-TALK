#include <rtthread.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#include "uvc_ai.h"
#include "model.h"
#ifdef UVC_AI_HAS_STEEL_BALL_MODEL
#include "steel_ball_model.h"
#endif
#include "mtb_ml.h"
#include "usb_config.h"

extern uint8_t *ml_shared_get_arena(uint32_t *size_out);
extern uint8_t *ml_shared_get_scratch(uint32_t *size_out);

#undef USB_DBG_TAG
#define USB_DBG_TAG "uvc_ai"
#include "usb_log.h"

#define UVC_AI_HALF(x)      ((x) * 0.5f)
#define UVC_AI_ROUND_FACTOR (0.5f)
#define UVC_AI_ITCM_SECTION __attribute__((section(".cy_itcm")))
#define UVC_AI_DTCM_SECTION __attribute__((section(".cy_dtcm")))
#define UVC_AI_ML_INPUT_SECTION __attribute__((section(".cy_ml_input_data")))
#define UVC_AI_ALIGN_16 __attribute__((aligned(16)))
#define UVC_AI_PAD_VALUE_U8 ((uint8_t)0)
#define UVC_AI_SRC_STRIDE_BYTES (UVC_AI_CAMERA_WIDTH * 2U)
#define UVC_AI_SRC_PAIR_COUNT (UVC_AI_CAMERA_WIDTH / 2U)
#define UVC_AI_FRAME_BYTES (UVC_AI_SRC_STRIDE_BYTES * UVC_AI_CAMERA_HEIGHT)
#define UVC_AI_TOP_RGB888_BYTES (UVC_AI_CAMERA_WIDTH * UVC_AI_CAMERA_HEIGHT * 3U)
#define UVC_AI_PAD_RGB888_BYTES ((UVC_AI_IMAGE_HEIGHT - UVC_AI_CAMERA_HEIGHT) * UVC_AI_IMAGE_WIDTH * 3U)
#define UVC_AI_MODEL_HEADER_WORD0 (0x00000024UL)
#define UVC_AI_MODEL_HEADER_WORD1 (0x334c4654UL)

static UVC_AI_ML_INPUT_SECTION UVC_AI_ALIGN_16 uint8_t g_model_input_u8[IMAI_DATA_IN_COUNT];
static UVC_AI_DTCM_SECTION float g_model_output[IMAI_DATA_OUT_COUNT];

static uint8_t g_ai_initialized;
static uvc_ai_mode_t g_ai_active_mode = UVC_AI_MODE_FATIGUE;
static uvc_ai_mode_t g_ai_init_mode = UVC_AI_MODE_FATIGUE;
static float g_steel_ball_conf_thresh = 0.65f;
#ifdef UVC_AI_HAS_STEEL_BALL_MODEL
static mtb_ml_model_t *g_steel_ball_model;
static uint8_t g_sb_fast_quant_input;
static uint8_t g_sb_hyperram_loaded;
static __attribute__((section(".m33_m55_shared_hyperram"), aligned(32)))
    uint8_t g_steel_ball_hyperram_bin[(STEEL_BALL_MODEL_BIN_SIZE + 31U) & ~31U];

typedef struct {
    int8_t *box_ptr;
    int8_t *cls_ptr;
    int32_t box_zp;
    float box_scale;
    int32_t cls_zp;
    float cls_scale;
    uint16_t hw;
    float stride;
} steel_ball_scale_desc_t;

static steel_ball_scale_desc_t g_sb_scales[3];
#endif
static uint16_t g_src_width;
static uint16_t g_src_height;
static uint64_t g_last_npu_cycles;
static uint32_t g_last_score_count;
static uint8_t g_restore_unalign_trp;
static uint32_t g_saved_ccr;
static uint8_t g_uvc_ai_yuv_lut_ready;
static rt_tick_t g_last_ai_log_tick;
static UVC_AI_DTCM_SECTION int32_t g_uvc_ai_y_lut[256];
static UVC_AI_DTCM_SECTION int32_t g_uvc_ai_u_to_b_lut[256];
static UVC_AI_DTCM_SECTION int32_t g_uvc_ai_u_to_g_lut[256];
static UVC_AI_DTCM_SECTION int32_t g_uvc_ai_v_to_r_lut[256];
static UVC_AI_DTCM_SECTION int32_t g_uvc_ai_v_to_g_lut[256];

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
        int32_t y = (int32_t)i - 16;
        int32_t uv = (int32_t)i - 128;

        if (y < 0) {
            y = 0;
        }

        g_uvc_ai_y_lut[i] = 298 * y;
        g_uvc_ai_u_to_b_lut[i] = 516 * uv;
        g_uvc_ai_u_to_g_lut[i] = -100 * uv;
        g_uvc_ai_v_to_r_lut[i] = 409 * uv;
        g_uvc_ai_v_to_g_lut[i] = -208 * uv;
    }

    g_uvc_ai_yuv_lut_ready = 1U;
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

static UVC_AI_ITCM_SECTION void uvc_ai_yuyv_to_rgb888_320x320_u8(const uint8_t *restrict yuyv)
{
    uint32_t y;
    uint8_t *restrict dst = g_model_input_u8;
    const int32_t *restrict y_lut = g_uvc_ai_y_lut;
    const int32_t *restrict u_to_b_lut = g_uvc_ai_u_to_b_lut;
    const int32_t *restrict u_to_g_lut = g_uvc_ai_u_to_g_lut;
    const int32_t *restrict v_to_r_lut = g_uvc_ai_v_to_r_lut;
    const int32_t *restrict v_to_g_lut = g_uvc_ai_v_to_g_lut;

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

void uvc_ai_set_conf_threshold(float thresh)
{
    if (thresh < 0.15f) thresh = 0.15f;
    if (thresh > 0.95f) thresh = 0.95f;
    g_steel_ball_conf_thresh = thresh;
}

float uvc_ai_get_conf_threshold(void)
{
    return g_steel_ball_conf_thresh;
}

#ifdef UVC_AI_HAS_STEEL_BALL_MODEL
static int uvc_ai_steel_ball_init(void)
{
    uint32_t arena_size = 0U;
    uint8_t *arena = ml_shared_get_arena(&arena_size);
    if (!g_sb_hyperram_loaded ||
        (memcmp(g_steel_ball_hyperram_bin, g_steel_ball_model_bin, 8U) != 0)) {
        memcpy(g_steel_ball_hyperram_bin, g_steel_ball_model_bin, STEEL_BALL_MODEL_BIN_SIZE);
        g_sb_hyperram_loaded = 1U;
    }

    mtb_ml_model_bin_t bin = {
        .name = "steel_ball_yolov5nu_320_int8",
        .model_bin = g_steel_ball_hyperram_bin,
        .model_size = STEEL_BALL_MODEL_BIN_SIZE,
        .arena_size = (int)arena_size,
    };
    mtb_ml_model_buffer_t buf = {
        .tensor_arena = arena,
        .tensor_arena_size = arena_size,
    };
    cy_rslt_t rslt;
    int i;
    int found_mask[3] = {0, 0, 0};

    if ((arena == RT_NULL) || (arena_size < STEEL_BALL_ARENA_SIZE)) {
        USB_LOG_ERR("SteelBall arena too small: %lu\r\n", (unsigned long)arena_size);
        return -RT_ENOMEM;
    }

    rslt = mtb_ml_model_init(&bin, &buf, &g_steel_ball_model);
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
        (g_steel_ball_model->input_size != IMAI_DATA_IN_COUNT) ||
        (g_steel_ball_model->output_count != 6)) {
        USB_LOG_ERR("SteelBall bad io meta: in=%d out_cnt=%d\r\n",
                    g_steel_ball_model->input_size, g_steel_ball_model->output_count);
        mtb_ml_model_deinit(g_steel_ball_model);
        g_steel_ball_model = RT_NULL;
        mtb_ml_deinit();
        return -RT_ERROR;
    }

    memset(g_sb_scales, 0, sizeof(g_sb_scales));
    for (i = 0; i < g_steel_ball_model->output_count; i++) {
        const mtb_ml_tensor_desc_t *td = &g_steel_ball_model->outputs[i];
        int scale_idx = -1;
        uint16_t hw = 0U;
        float stride = 0.0f;

        if ((td->dim_len != 4) || (td->dims == RT_NULL) || (td->dims[1] != td->dims[2])) {
            continue;
        }
        if (td->dims[1] == 40) { scale_idx = 0; hw = 40U; stride = 8.0f; }
        else if (td->dims[1] == 20) { scale_idx = 1; hw = 20U; stride = 16.0f; }
        else if (td->dims[1] == 10) { scale_idx = 2; hw = 10U; stride = 32.0f; }
        else continue;

        g_sb_scales[scale_idx].hw = hw;
        g_sb_scales[scale_idx].stride = stride;
        if (td->dims[3] == 64) {
            g_sb_scales[scale_idx].box_ptr = (int8_t *)td->ptr;
            g_sb_scales[scale_idx].box_zp = td->zero_point;
            g_sb_scales[scale_idx].box_scale = td->scale;
            found_mask[scale_idx] |= 1;
        } else if (td->dims[3] == 1) {
            g_sb_scales[scale_idx].cls_ptr = (int8_t *)td->ptr;
            g_sb_scales[scale_idx].cls_zp = td->zero_point;
            g_sb_scales[scale_idx].cls_scale = td->scale;
            found_mask[scale_idx] |= 2;
        }
    }

    if ((found_mask[0] != 3) || (found_mask[1] != 3) || (found_mask[2] != 3)) {
        USB_LOG_ERR("SteelBall output tensors mismatch: %d %d %d\r\n",
                    found_mask[0], found_mask[1], found_mask[2]);
        mtb_ml_model_deinit(g_steel_ball_model);
        g_steel_ball_model = RT_NULL;
        mtb_ml_deinit();
        return -RT_ERROR;
    }

    g_sb_fast_quant_input = (uint8_t)((g_steel_ball_model->input_zero_point == -128) &&
                                      (fabsf(g_steel_ball_model->input_scale - (1.0f / 255.0f)) < 1e-5f));
#if defined(__DCACHE_PRESENT) && (__DCACHE_PRESENT == 1U)
    if ((SCB->CCR & SCB_CCR_DC_Msk) != 0U) {
        SCB_CleanInvalidateDCache();
    }
#endif

    USB_LOG_INFO("SteelBall model initialized (arena used=%d B, fast_in=%u)\r\n",
                 g_steel_ball_model->buffer_size, (unsigned)g_sb_fast_quant_input);
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

#define SB_MAX_CANDIDATES 128U

static UVC_AI_ITCM_SECTION void uvc_ai_yuyv_to_s8_320x240(const uint8_t *restrict yuyv,
                                                           int8_t *restrict dst)
{
    uint32_t y;
    const int32_t *restrict y_lut = g_uvc_ai_y_lut;
    const int32_t *restrict u_to_b_lut = g_uvc_ai_u_to_b_lut;
    const int32_t *restrict u_to_g_lut = g_uvc_ai_u_to_g_lut;
    const int32_t *restrict v_to_r_lut = g_uvc_ai_v_to_r_lut;
    const int32_t *restrict v_to_g_lut = g_uvc_ai_v_to_g_lut;

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

static int uvc_ai_steel_ball_compute(uvc_ai_result_t *result)
{
    int8_t *in_buf = (int8_t *)g_steel_ball_model->input;
    uint32_t scratch_size = 0U;
    uint8_t *scratch = ml_shared_get_scratch(&scratch_size);
    cy_rslt_t rslt;
    int s_idx;
    sb_cand_t *cands;
    uint8_t *suppressed;
    uint32_t cand_count = 0U;
    float thresh = g_steel_ball_conf_thresh;
    float logit_thresh;
    uint32_t i;
    uint32_t j;

    if ((scratch == RT_NULL) ||
        (scratch_size < (SB_MAX_CANDIDATES * sizeof(sb_cand_t) + SB_MAX_CANDIDATES))) {
        return -RT_ENOMEM;
    }
    cands = (sb_cand_t *)scratch;
    suppressed = scratch + SB_MAX_CANDIDATES * sizeof(sb_cand_t);

    if (g_sb_fast_quant_input) {
        /* The tensor arena can reuse input storage during inference. Fill the
         * entire input for every invocation, including the letterbox padding. */
        memset(in_buf + UVC_AI_TOP_RGB888_BYTES, 0x80, UVC_AI_PAD_RGB888_BYTES);
    } else {
        uvc_ai_quantize_u8_to_s8(g_model_input_u8,
                                 in_buf,
                                 IMAI_DATA_IN_COUNT,
                                 g_steel_ball_model->input_scale,
                                 g_steel_ball_model->input_zero_point);
    }

#if defined(__DCACHE_PRESENT) && (__DCACHE_PRESENT == 1U)
    if ((SCB->CCR & SCB_CCR_DC_Msk) != 0U) {
        uintptr_t p = (uintptr_t)in_buf;
        uintptr_t aligned_start = p & ~(uintptr_t)31U;
        uintptr_t aligned_end = (p + IMAI_DATA_IN_COUNT + 31U) & ~(uintptr_t)31U;
        SCB_CleanDCache_by_Addr((void *)aligned_start, (int32_t)(aligned_end - aligned_start));
    }
#endif

    rslt = mtb_ml_model_invoke(g_steel_ball_model);
    if (rslt != CY_RSLT_SUCCESS) {
        USB_LOG_ERR("SteelBall invoke failed: 0x%08lx\r\n", (unsigned long)rslt);
        return -RT_ERROR;
    }

#if defined(__DCACHE_PRESENT) && (__DCACHE_PRESENT == 1U)
    if ((SCB->CCR & SCB_CCR_DC_Msk) != 0U) {
        int out_k;
        for (out_k = 0; out_k < g_steel_ball_model->output_count; out_k++) {
            uintptr_t p = (uintptr_t)g_steel_ball_model->outputs[out_k].ptr;
            size_t b = g_steel_ball_model->outputs[out_k].bytes;
            uintptr_t aligned_start = p & ~(uintptr_t)31U;
            uintptr_t aligned_end = (p + b + 31U) & ~(uintptr_t)31U;
            SCB_InvalidateDCache_by_Addr((void *)aligned_start, (int32_t)(aligned_end - aligned_start));
        }
    }
#endif

    if (thresh <= 0.01f) thresh = 0.01f;
    if (thresh >= 0.99f) thresh = 0.99f;
    logit_thresh = logf(thresh / (1.0f - thresh));

    for (s_idx = 0; s_idx < 3; s_idx++) {
        const steel_ball_scale_desc_t *sc = &g_sb_scales[s_idx];
        uint32_t hw = sc->hw;
        float stride = sc->stride;
        float raw_f = (logit_thresh / sc->cls_scale) + (float)sc->cls_zp;
        int32_t raw_min = (int32_t)floorf(raw_f);
        uint32_t gy;

        for (gy = 0U; gy < hw; gy++) {
            uint32_t gx;
            if (((float)gy + 0.5f) * stride >= UVC_AI_CAMERA_HEIGHT)
                break;
            for (gx = 0U; gx < hw; gx++) {
                uint32_t cell = gy * hw + gx;
                int32_t q_cls = (int32_t)sc->cls_ptr[cell];
                float logit;
                float prob;
                const int8_t *box_base;
                float dists[4];
                int side;

                if (q_cls < raw_min) {
                    continue;
                }

                logit = ((float)q_cls - (float)sc->cls_zp) * sc->cls_scale;
                prob = 1.0f / (1.0f + expf(-logit));
                if (prob < thresh) {
                    continue;
                }

                if (cand_count >= SB_MAX_CANDIDATES) {
                    continue;
                }

                box_base = sc->box_ptr + (cell * (4U * STEEL_BALL_DFL_REG_MAX));
                for (side = 0; side < 4; side++) {
                    const int8_t *bptr = box_base + (uint32_t)side * STEEL_BALL_DFL_REG_MAX;
                    int8_t q_max = bptr[0];
                    float exps[STEEL_BALL_DFL_REG_MAX];
                    float sum_exp = 0.0f;
                    float weighted = 0.0f;
                    uint32_t k;

                    for (k = 1U; k < STEEL_BALL_DFL_REG_MAX; k++) {
                        if (bptr[k] > q_max) q_max = bptr[k];
                    }
                    for (k = 0U; k < STEEL_BALL_DFL_REG_MAX; k++) {
                        float v = expf((float)((int32_t)bptr[k] - (int32_t)q_max) * sc->box_scale);
                        exps[k] = v;
                        sum_exp += v;
                    }
                    if (sum_exp > 0.0f) {
                        float inv_sum = 1.0f / sum_exp;
                        for (k = 1U; k < STEEL_BALL_DFL_REG_MAX; k++) {
                            weighted += (float)k * (exps[k] * inv_sum);
                        }
                    }
                    dists[side] = weighted;
                }

                cands[cand_count].x1 = fmaxf(0.0f, ((float)gx + 0.5f - dists[0]) * stride);
                cands[cand_count].y1 = fmaxf(0.0f, ((float)gy + 0.5f - dists[1]) * stride);
                cands[cand_count].x2 = fminf(UVC_AI_CAMERA_WIDTH - 1.0f,
                                             ((float)gx + 0.5f + dists[2]) * stride);
                cands[cand_count].y2 = fminf(UVC_AI_CAMERA_HEIGHT - 1.0f,
                                             ((float)gy + 0.5f + dists[3]) * stride);
                if (lroundf(cands[cand_count].x2) <= lroundf(cands[cand_count].x1) ||
                    lroundf(cands[cand_count].y2) <= lroundf(cands[cand_count].y1))
                    continue;
                cands[cand_count].score = prob;
                cand_count++;
            }
        }
    }

    /* Sort descending by confidence */
    for (i = 0U; i + 1U < cand_count; i++) {
        uint32_t max_i = i;
        for (j = i + 1U; j < cand_count; j++) {
            if (cands[j].score > cands[max_i].score) {
                max_i = j;
            }
        }
        if (max_i != i) {
            sb_cand_t tmp = cands[i];
            cands[i] = cands[max_i];
            cands[max_i] = tmp;
        }
    }

    memset(suppressed, 0, cand_count);
    for (i = 0U; (i < cand_count) && (result->count < UVC_AI_MAX_PREDICTIONS); i++) {
        float ax1;
        float ay1;
        float ax2;
        float ay2;
        float area_a;
        uint32_t out_idx;

        if (suppressed[i]) {
            continue;
        }

        ax1 = cands[i].x1;
        ay1 = cands[i].y1;
        ax2 = cands[i].x2;
        ay2 = cands[i].y2;
        area_a = (ax2 > ax1 && ay2 > ay1) ? ((ax2 - ax1) * (ay2 - ay1)) : 0.0f;

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
            union_a = area_a + area_b - inter;
            if ((union_a > 0.0f) && ((inter / union_a) > 0.45f)) {
                suppressed[j] = 1U;
            }
        }

        out_idx = result->count;
        (void)snprintf(result->class_string[out_idx], UVC_AI_MAX_CLASS_LEN, "SteelBall");
        result->class_id[out_idx] = 0U;
        result->conf[out_idx] = cands[i].score;
        result->bbox_int16[out_idx * 4U + 0U] = uvc_ai_clamp_i16((int32_t)lroundf(ax1), 0, (int32_t)UVC_AI_CAMERA_WIDTH - 1);
        result->bbox_int16[out_idx * 4U + 1U] = uvc_ai_clamp_i16((int32_t)lroundf(ay1), 0, (int32_t)UVC_AI_CAMERA_HEIGHT - 1);
        result->bbox_int16[out_idx * 4U + 2U] = uvc_ai_clamp_i16((int32_t)lroundf(ax2), 0, (int32_t)UVC_AI_CAMERA_WIDTH - 1);
        result->bbox_int16[out_idx * 4U + 3U] = uvc_ai_clamp_i16((int32_t)lroundf(ay2), 0, (int32_t)UVC_AI_CAMERA_HEIGHT - 1);
        result->count = (uint8_t)(out_idx + 1U);
    }

    result->valid = 1U;
    return RT_EOK;
}

#endif

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

#ifndef UVC_AI_HAS_STEEL_BALL_MODEL
    if (g_ai_active_mode == UVC_AI_MODE_STEEL_BALL) {
        USB_LOG_WRN("Install the steel-ball model to enable steel-ball detection\r\n");
        return -RT_ENOSYS;
    }
#endif

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

#ifdef UVC_AI_HAS_STEEL_BALL_MODEL
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
    } else
#endif
    {
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
     * Keep the bottom band pre-filled once instead of clearing every frame.
     */
    memset(g_model_input_u8 + UVC_AI_TOP_RGB888_BYTES,
           UVC_AI_PAD_VALUE_U8,
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

#ifdef UVC_AI_HAS_STEEL_BALL_MODEL
    if (g_ai_init_mode == UVC_AI_MODE_STEEL_BALL) {
        uvc_ai_steel_ball_deinit();
    } else
#endif
    {
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

UVC_AI_ITCM_SECTION int uvc_ai_process_yuyv(const uint8_t *yuyv, uint32_t yuyv_size, uvc_ai_result_t *result)
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

    if ((!g_ai_initialized) || (yuyv == RT_NULL) || (result == RT_NULL)) {
        return -RT_EINVAL;
    }

    min_frame_size = UVC_AI_FRAME_BYTES;
    if (yuyv_size < min_frame_size) {
        USB_LOG_WRN("AI frame too small: %lu < %lu\r\n",
                    (unsigned long)yuyv_size, (unsigned long)min_frame_size);
        return -RT_EINVAL;
    }

    prep_start_tick = rt_tick_get();
#ifdef UVC_AI_HAS_STEEL_BALL_MODEL
    if ((g_ai_init_mode == UVC_AI_MODE_STEEL_BALL) && g_sb_fast_quant_input) {
        uvc_ai_yuyv_to_s8_320x240(yuyv, (int8_t *)g_steel_ball_model->input);
    } else
#endif
    {
        uvc_ai_yuyv_to_rgb888_320x320_u8(yuyv);
    }
    prep_end_tick = rt_tick_get();

    uvc_ai_reset_result(result);

    npu_cycles_before = mtb_ml_npu_cycles;
    start_tick = rt_tick_get();
#ifdef UVC_AI_HAS_STEEL_BALL_MODEL
    if (g_ai_init_mode == UVC_AI_MODE_STEEL_BALL) {
        int sb_ret = uvc_ai_steel_ball_compute(result);
        end_tick = rt_tick_get();
        npu_cycles_after = mtb_ml_npu_cycles;
        if (sb_ret != RT_EOK) {
            return sb_ret;
        }
        goto post_infer;
    }
#endif
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

        out_idx = result->count;
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

        (void)snprintf(result->class_string[out_idx], UVC_AI_MAX_CLASS_LEN, "%s", label);
        result->class_id[out_idx] = best_class;
        result->conf[out_idx] = best_score;

        xmin = (int32_t)((x - UVC_AI_HALF(w)) * UVC_AI_IMAGE_WIDTH + UVC_AI_ROUND_FACTOR);
        ymin = (int32_t)((y - UVC_AI_HALF(h)) * UVC_AI_IMAGE_WIDTH + UVC_AI_ROUND_FACTOR);
        xmax = (int32_t)((x + UVC_AI_HALF(w)) * UVC_AI_IMAGE_WIDTH + UVC_AI_ROUND_FACTOR);
        ymax = (int32_t)((y + UVC_AI_HALF(h)) * UVC_AI_IMAGE_WIDTH + UVC_AI_ROUND_FACTOR);

        result->bbox_int16[out_idx * 4U + 0U] = uvc_ai_clamp_i16(xmin, 0, (int32_t)UVC_AI_IMAGE_WIDTH - 1);
        result->bbox_int16[out_idx * 4U + 1U] = uvc_ai_clamp_i16(ymin, 0, (int32_t)UVC_AI_IMAGE_HEIGHT - 1);
        result->bbox_int16[out_idx * 4U + 2U] = uvc_ai_clamp_i16(xmax, 0, (int32_t)UVC_AI_IMAGE_WIDTH - 1);
        result->bbox_int16[out_idx * 4U + 3U] = uvc_ai_clamp_i16(ymax, 0, (int32_t)UVC_AI_IMAGE_HEIGHT - 1);
        result->count = (uint8_t)(out_idx + 1U);
    }

    result->valid = 1U;
post_infer:
    prep_ms = ((float)(prep_end_tick - prep_start_tick) * 1000.0f) / (float)RT_TICK_PER_SECOND;
    result->inference_ms = ((float)(end_tick - start_tick) * 1000.0f) / (float)RT_TICK_PER_SECOND;

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
        USB_LOG_INFO("AI inference %.1f ms (prep %.1f ms, npu %.1f ms), det=%u\r\n",
                     result->inference_ms, prep_ms, npu_ms, result->count);
        for (i = 0U; i < result->count; i++) {
            const char *risk = (strncmp(result->class_string[i], "Closed", 6U) == 0)
                               ? uvc_ai_risk_label(result->conf[i]) : "";
            USB_LOG_INFO("AI #%lu %s %.2f%% %s bbox=[%d,%d,%d,%d]\r\n",
                         (unsigned long)i,
                         result->class_string[i],
                         result->conf[i] * 100.0f,
                         risk,
                         result->bbox_int16[i * 4U + 0U],
                         result->bbox_int16[i * 4U + 1U],
                         result->bbox_int16[i * 4U + 2U],
                         result->bbox_int16[i * 4U + 3U]);
        }
        g_last_ai_log_tick = log_now_tick;
    }

    return RT_EOK;
}
