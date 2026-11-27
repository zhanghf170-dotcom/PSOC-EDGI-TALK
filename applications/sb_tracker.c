#include "sb_tracker.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

#define SB_FRAME_MAX_X          ((float)(UVC_AI_CAMERA_WIDTH - 1U))
#define SB_FRAME_MAX_Y          ((float)(UVC_AI_CAMERA_HEIGHT - 1U))

#define SB_RESET_GAP_MS         500U
#define SB_DEFAULT_DT_S         0.08f
#define SB_MIN_DT_S             0.02f
#define SB_MAX_DT_S             0.50f

#define SB_GATE_MAX_DNORM       2.0f
#define SB_GATE_MIN_SIZE_RATIO  0.5f
#define SB_GATE_MAX_SIZE_RATIO  2.0f
#define SB_MIN_AVG_SIZE         12.0f
#define SB_DNORM_COST_SCALE     2.5f
#define SB_MAX_ASSOC_COST       1.15f
#define SB_COST_INVALID         1.0e9f
#define SB_ASSOC_COST_SCALE     4096
#define SB_ASSOC_UNMATCHED      262144
#define SB_ASSOC_FORBIDDEN      (2 * SB_ASSOC_UNMATCHED)
#define SB_ASSOC_MAX_COLUMNS    (SB_TRACKER_MAX_TRACKS + UVC_AI_MAX_PREDICTIONS)

#define SB_ALPHA_POS            0.65f
#define SB_BETA_VEL             0.25f
#define SB_GAMMA_SIZE           0.35f
#define SB_MAX_SPEED_PX_S       600.0f

#define SB_INSTANT_CONFIRM_CONF 0.85f
#define SB_SINGLE_CONFIRM_CONF  0.78f
#define SB_SINGLE_MAX_LEAD_MS   100U
#define SB_SINGLE_MAX_LEAD_PX   42
#define SB_COAST_MAX_MISSED     3U
#define SB_TENTATIVE_MAX_MISSED 2U
#define SB_CONFIRM_MAX_AGE      3U
#define SB_COAST_VEL_DECAY      0.85f
#define SB_COAST_CONF_DECAY     0.92f

typedef struct {
    float cx;
    float cy;
    float w;
    float h;
    float conf;
} sb_det_t;

static float sb_clampf(float v, float lo, float hi)
{
    if (!(v >= lo)) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

static int16_t sb_round_clamp_i16(float v, int32_t lo, int32_t hi)
{
    int32_t iv;
    if (!isfinite(v)) {
        return (int16_t)lo;
    }
    iv = (v >= 0.0f) ? (int32_t)(v + 0.5f) : (int32_t)(v - 0.5f);
    if (iv < lo) {
        iv = lo;
    }
    if (iv > hi) {
        iv = hi;
    }
    return (int16_t)iv;
}

static uint8_t sb_extract_dets(const uvc_ai_result_t *res, sb_det_t *dets)
{
    uint8_t count;
    uint8_t i;

    if (res == NULL) {
        return 0U;
    }

    count = res->count;
    if (count > (uint8_t)UVC_AI_MAX_PREDICTIONS) {
        count = (uint8_t)UVC_AI_MAX_PREDICTIONS;
    }

    for (i = 0U; i < count; i++) {
        const int16_t *box = &res->bbox_int16[(uint32_t)i * 4U];
        float x1 = (float)box[0];
        float y1 = (float)box[1];
        float x2 = (float)box[2];
        float y2 = (float)box[3];
        float w;
        float h;

        if (x1 > x2) {
            float tmp = x1;
            x1 = x2;
            x2 = tmp;
        }
        if (y1 > y2) {
            float tmp = y1;
            y1 = y2;
            y2 = tmp;
        }

        w = x2 - x1;
        h = y2 - y1;
        if (w < 1.0f) {
            w = 1.0f;
        }
        if (h < 1.0f) {
            h = 1.0f;
        }

        dets[i].cx = 0.5f * (x1 + x2);
        dets[i].cy = 0.5f * (y1 + y2);
        dets[i].w = w;
        dets[i].h = h;
        dets[i].conf = sb_clampf(res->conf[i], 0.0f, 1.0f);
    }

    return count;
}

static float sb_assoc_cost(float acx, float acy, float aw, float ah,
                           const sb_det_t *det, uint8_t low_confidence, float dt,
                           uint8_t fast_single)
{
    float aw_safe = (aw > 1.0f) ? aw : 1.0f;
    float ah_safe = (ah > 1.0f) ? ah : 1.0f;
    float bw_safe = (det->w > 1.0f) ? det->w : 1.0f;
    float bh_safe = (det->h > 1.0f) ? det->h : 1.0f;
    float w_ratio = bw_safe / aw_safe;
    float h_ratio = bh_safe / ah_safe;
    float avg_s;
    float denom;
    float dx;
    float dy;
    float d2;
    float max_d;
    float d_norm;
    float ax1, ay1, ax2, ay2;
    float bx1, by1, bx2, by2;
    float ix1, iy1, ix2, iy2;
    float iw, ih, inter_area;
    float area_a, area_b, union_area;
    float ex1, ey1, ex2, ey2, enclosing_area;
    float iou, giou, cost;

    if ((w_ratio < SB_GATE_MIN_SIZE_RATIO) || (w_ratio > SB_GATE_MAX_SIZE_RATIO) ||
        (h_ratio < SB_GATE_MIN_SIZE_RATIO) || (h_ratio > SB_GATE_MAX_SIZE_RATIO)) {
        return SB_COST_INVALID;
    }

    avg_s = 0.5f * (0.5f * (aw_safe + ah_safe) + 0.5f * (bw_safe + bh_safe));
    denom = (avg_s > SB_MIN_AVG_SIZE) ? avg_s : SB_MIN_AVG_SIZE;
    dx = det->cx - acx;
    dy = det->cy - acy;
    d2 = dx * dx + dy * dy;
    /* Weak detections may extend a track only near its predicted position. */
    max_d = (low_confidence ? 1.25f : SB_GATE_MAX_DNORM) * denom;
    if (fast_single) {
        max_d = fmaxf(max_d, fminf(56.0f, 12.0f + 320.0f * dt));
        denom = fmaxf(denom, 0.5f * max_d);
    }
    if (!isfinite(d2) || (d2 > (max_d * max_d))) {
        return SB_COST_INVALID;
    }
    d_norm = sqrtf(d2) / denom;
    if (!isfinite(d_norm) || (d_norm > SB_GATE_MAX_DNORM)) {
        return SB_COST_INVALID;
    }

    ax1 = acx - 0.5f * aw_safe;
    ay1 = acy - 0.5f * ah_safe;
    ax2 = acx + 0.5f * aw_safe;
    ay2 = acy + 0.5f * ah_safe;

    bx1 = det->cx - 0.5f * bw_safe;
    by1 = det->cy - 0.5f * bh_safe;
    bx2 = det->cx + 0.5f * bw_safe;
    by2 = det->cy + 0.5f * bh_safe;

    ix1 = (ax1 > bx1) ? ax1 : bx1;
    iy1 = (ay1 > by1) ? ay1 : by1;
    ix2 = (ax2 < bx2) ? ax2 : bx2;
    iy2 = (ay2 < by2) ? ay2 : by2;

    iw = (ix2 > ix1) ? (ix2 - ix1) : 0.0f;
    ih = (iy2 > iy1) ? (iy2 - iy1) : 0.0f;
    inter_area = iw * ih;

    area_a = aw_safe * ah_safe;
    area_b = bw_safe * bh_safe;
    union_area = area_a + area_b - inter_area;
    iou = (union_area > 1.0e-6f) ? (inter_area / union_area) : 0.0f;

    ex1 = (ax1 < bx1) ? ax1 : bx1;
    ey1 = (ay1 < by1) ? ay1 : by1;
    ex2 = (ax2 > bx2) ? ax2 : bx2;
    ey2 = (ay2 > by2) ? ay2 : by2;
    enclosing_area = (ex2 - ex1) * (ey2 - ey1);

    if (enclosing_area > 1.0e-6f) {
        giou = iou - (enclosing_area - union_area) / enclosing_area;
    } else {
        giou = iou;
    }

    {
        float dist_term = d_norm / SB_DNORM_COST_SCALE;
        if (dist_term > 1.0f) {
            dist_term = 1.0f;
        }
        cost = 0.5f * (1.0f - giou) + 0.5f * dist_term;
    }

    if (!isfinite(cost) || (cost > SB_MAX_ASSOC_COST)) {
        return SB_COST_INVALID;
    }
    return cost;
}

static void sb_update_matched_track(sb_track_t *t,
                                    float pred_cx,
                                    float pred_cy,
                                    const sb_det_t *det,
                                    float dt,
                                    uint8_t is_stage1,
                                    uint8_t fast_single)
{
    float rx = det->cx - pred_cx;
    float ry = det->cy - pred_cy;
    float speed;
    float ema_conf;

    uint8_t moving = fast_single && (rx * rx + ry * ry >= 4.0f);
    float alpha = moving ? 0.90f : SB_ALPHA_POS;
    float beta = moving ? (t->hits == 1U ? 0.75f : 0.50f) : SB_BETA_VEL;
    t->cx = sb_clampf(pred_cx + alpha * rx, 0.0f, SB_FRAME_MAX_X);
    t->cy = sb_clampf(pred_cy + alpha * ry, 0.0f, SB_FRAME_MAX_Y);

    t->vx += (beta / dt) * rx;
    t->vy += (beta / dt) * ry;
    if (!isfinite(t->vx) || !isfinite(t->vy)) {
        t->vx = 0.0f;
        t->vy = 0.0f;
    } else {
        float speed2 = t->vx * t->vx + t->vy * t->vy;
        if (!isfinite(speed2)) {
            t->vx = 0.0f;
            t->vy = 0.0f;
        } else if (speed2 > (SB_MAX_SPEED_PX_S * SB_MAX_SPEED_PX_S)) {
            speed = sqrtf(speed2);
            if (!isfinite(speed) || (speed <= 0.0f)) {
                t->vx = 0.0f;
                t->vy = 0.0f;
            } else {
                float scale = SB_MAX_SPEED_PX_S / speed;
                t->vx *= scale;
                t->vy *= scale;
            }
        }
    }

    t->w = (1.0f - SB_GAMMA_SIZE) * t->w + SB_GAMMA_SIZE * det->w;
    t->h = (1.0f - SB_GAMMA_SIZE) * t->h + SB_GAMMA_SIZE * det->h;
    if (!isfinite(t->w) || (t->w < 1.0f)) {
        t->w = 1.0f;
    }
    if (!isfinite(t->h) || (t->h < 1.0f)) {
        t->h = 1.0f;
    }

    ema_conf = 0.5f * t->conf + 0.5f * det->conf;
    if (is_stage1 != 0U) {
        t->conf = (det->conf > ema_conf) ? det->conf : ema_conf;
    } else {
        t->conf = ema_conf;
    }
    t->conf = sb_clampf(t->conf, 0.0f, 1.0f);

    if (t->hits < 255U) {
        t->hits++;
    }
    if (t->age < 255U) {
        t->age++;
    }
    t->missed = 0U;

    if (t->confirmed == 0U) {
        if (((t->hits >= 2U) && (t->age <= SB_CONFIRM_MAX_AGE)) ||
            ((is_stage1 != 0U) && (det->conf >= SB_INSTANT_CONFIRM_CONF))) {
            t->confirmed = 1U;
        }
    }
}

static void sb_match_stage(sb_tracker_t *tr,
                           const float *pred_cx, const float *pred_cy,
                           const sb_det_t *dets, uint8_t det_count, float dt,
                           uint8_t confirmed_only, uint8_t fast_single, uint8_t *track_matched,
                           uint8_t *det_matched)
{
    uint8_t tracks[SB_TRACKER_MAX_TRACKS];
    uint8_t rows = 0U;
    int32_t costs[SB_TRACKER_MAX_TRACKS][UVC_AI_MAX_PREDICTIONS];

    if (det_count == 0U) {
        return;
    }

    for (uint8_t t = 0U; t < SB_TRACKER_MAX_TRACKS; t++) {
        const sb_track_t *track = &tr->tracks[t];
        if (!track->active || track_matched[t] || (confirmed_only && !track->confirmed)) {
            continue;
        }

        tracks[rows] = t;
        uint8_t valid = 0U;
        for (uint8_t d = 0U; d < det_count; d++) {
            float cost = sb_assoc_cost(pred_cx[t], pred_cy[t], track->w, track->h,
                                       &dets[d], confirmed_only, dt, fast_single);
            if (cost <= SB_MAX_ASSOC_COST) {
                costs[rows][d] = (int32_t)(cost * SB_ASSOC_COST_SCALE + 0.5f) + !track->confirmed;
                valid = 1U;
            } else {
                costs[rows][d] = SB_ASSOC_FORBIDDEN;
            }
        }
        rows += valid;
    }

    if (rows == 0U) {
        return;
    }

    /* The unambiguous fast path avoids solving a 1x2 assignment. */
    if (rows == 1U && det_count == 1U) {
        uint8_t t = tracks[0];
        track_matched[t] = det_matched[0] = 1U;
        sb_update_matched_track(&tr->tracks[t], pred_cx[t], pred_cy[t], &dets[0],
                                dt, !confirmed_only, fast_single);
        return;
    }

    /* Rectangular Hungarian assignment. Dummy columns allow any track to coast.
     * The dummy penalty exceeds the sum of all legal costs, so gates first
     * maximize the number of matches and then minimize total distance. */
    int32_t row_potential[SB_TRACKER_MAX_TRACKS + 1U] = {0};
    int32_t col_potential[SB_ASSOC_MAX_COLUMNS + 1U] = {0};
    uint8_t owner[SB_ASSOC_MAX_COLUMNS + 1U] = {0};
    uint8_t predecessor[SB_ASSOC_MAX_COLUMNS + 1U];
    uint8_t cols = det_count + rows;

    for (uint8_t row = 1U; row <= rows; row++) {
        int32_t min_cost[SB_ASSOC_MAX_COLUMNS + 1U];
        uint8_t visited[SB_ASSOC_MAX_COLUMNS + 1U] = {0};
        uint8_t col = 0U;

        owner[0] = row;
        for (uint8_t j = 1U; j <= cols; j++) {
            min_cost[j] = INT32_MAX;
        }
        do {
            uint8_t current = owner[col];
            uint8_t next_col = 0U;
            int32_t delta = INT32_MAX;

            visited[col] = 1U;
            for (uint8_t j = 1U; j <= cols; j++) {
                if (visited[j]) {
                    continue;
                }
                int32_t reduced = (j <= det_count ? costs[current - 1U][j - 1U] : SB_ASSOC_UNMATCHED)
                                  - row_potential[current] - col_potential[j];
                if (reduced < min_cost[j]) {
                    min_cost[j] = reduced;
                    predecessor[j] = col;
                }
                if (min_cost[j] < delta) {
                    delta = min_cost[j];
                    next_col = j;
                }
            }
            for (uint8_t j = 0U; j <= cols; j++) {
                if (visited[j]) {
                    row_potential[owner[j]] += delta;
                    col_potential[j] -= delta;
                } else {
                    min_cost[j] -= delta;
                }
            }
            col = next_col;
        } while (owner[col] != 0U);

        do {
            uint8_t prev = predecessor[col];
            owner[col] = owner[prev];
            col = prev;
        } while (col != 0U);
    }

    for (uint8_t d = 0U; d < det_count; d++) {
        uint8_t row = owner[d + 1U];
        if (row == 0U || costs[row - 1U][d] == SB_ASSOC_FORBIDDEN) {
            continue;
        }
        uint8_t t = tracks[row - 1U];
        track_matched[t] = 1U;
        det_matched[d] = 1U;
        sb_update_matched_track(&tr->tracks[t], pred_cx[t], pred_cy[t], &dets[d],
                                dt, !confirmed_only, fast_single);
    }
}

static uint8_t sb_alloc_track_slot(const sb_tracker_t *tr)
{
    uint8_t i;
    uint8_t victim = SB_TRACKER_MAX_TRACKS;

    for (i = 0U; i < SB_TRACKER_MAX_TRACKS; i++) {
        if (tr->tracks[i].active == 0U) {
            return i;
        }
    }

    for (i = 0U; i < SB_TRACKER_MAX_TRACKS; i++) {
        const sb_track_t *cand = &tr->tracks[i];
        if (cand->confirmed != 0U || cand->missed == 0U) {
            continue;
        }
        if (victim == SB_TRACKER_MAX_TRACKS) {
            victim = i;
        } else {
            const sb_track_t *cur = &tr->tracks[victim];
            if ((cand->age > cur->age) ||
                ((cand->age == cur->age) && (cand->missed > cur->missed)) ||
                ((cand->age == cur->age) && (cand->missed == cur->missed) && (cand->conf < cur->conf)) ||
                ((cand->age == cur->age) && (cand->missed == cur->missed) &&
                 (cand->conf == cur->conf) && (cand->id < cur->id))) {
                victim = i;
            }
        }
    }

    return victim;
}

static uint16_t sb_next_track_id(sb_tracker_t *tr)
{
    for (;;) {
        uint16_t id = tr->next_id++;
        if (id == 0U) {
            continue;
        }
        uint8_t i;
        for (i = 0U; i < SB_TRACKER_MAX_TRACKS; i++) {
            if (tr->tracks[i].active && tr->tracks[i].id == id) {
                break;
            }
        }
        if (i == SB_TRACKER_MAX_TRACKS) {
            return id;
        }
    }
}

static void sb_copy_class_label(char *dst, size_t dst_size, const char *src)
{
    size_t i = 0U;
    if ((dst == NULL) || (dst_size == 0U)) {
        return;
    }
    while ((i + 1U < dst_size) && (src[i] != '\0')) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

void sb_tracker_init(sb_tracker_t *tr)
{
    if (tr == NULL) {
        return;
    }
    memset(tr, 0, sizeof(*tr));
    tr->next_id = 1U;
}

void sb_tracker_reset(sb_tracker_t *tr)
{
    sb_tracker_init(tr);
}

void sb_tracker_update(sb_tracker_t *tr,
                       const uvc_ai_result_t *high_dets,
                       const uvc_ai_result_t *low_dets,
                       uint32_t now_ms,
                       uvc_ai_result_t *out)
{
    sb_det_t high_list[UVC_AI_MAX_PREDICTIONS];
    sb_det_t low_list[UVC_AI_MAX_PREDICTIONS];
    uint8_t high_count;
    uint8_t low_count;
    float raw_pred_cx[SB_TRACKER_MAX_TRACKS];
    float raw_pred_cy[SB_TRACKER_MAX_TRACKS];
    float pred_cx[SB_TRACKER_MAX_TRACKS];
    float pred_cy[SB_TRACKER_MAX_TRACKS];
    uint8_t track_matched[SB_TRACKER_MAX_TRACKS];
    uint8_t high_matched[UVC_AI_MAX_PREDICTIONS];
    uint8_t low_matched[UVC_AI_MAX_PREDICTIONS];
    uint8_t emit_order[SB_TRACKER_MAX_TRACKS];
    uint8_t emit_count = 0U;
    uint8_t active_count = 0U;
    uint8_t i;
    float dt;

    if ((tr == NULL) || (out == NULL)) {
        return;
    }

    memset(out, 0, sizeof(*out));
    memset(track_matched, 0, sizeof(track_matched));
    memset(high_matched, 0, sizeof(high_matched));
    memset(low_matched, 0, sizeof(low_matched));

    if ((tr->has_last_ts == 0U) || ((uint32_t)(now_ms - tr->last_ms) > SB_RESET_GAP_MS)) {
        memset(tr->tracks, 0, sizeof(tr->tracks));
        dt = SB_DEFAULT_DT_S;
    } else {
        uint32_t elapsed_ms = (uint32_t)(now_ms - tr->last_ms);
        dt = sb_clampf((float)elapsed_ms * 0.001f, SB_MIN_DT_S, SB_MAX_DT_S);
    }
    tr->last_ms = now_ms;
    tr->has_last_ts = 1U;

    high_count = sb_extract_dets(high_dets, high_list);
    low_count = sb_extract_dets(low_dets, low_list);

    /* 1. Predict step for all active tracks */
    for (i = 0U; i < SB_TRACKER_MAX_TRACKS; i++) {
        sb_track_t *t = &tr->tracks[i];
        if (t->active != 0U) {
            if (!isfinite(t->cx) || !isfinite(t->cy) ||
                !isfinite(t->vx) || !isfinite(t->vy) ||
                !isfinite(t->w) || !isfinite(t->h) || !isfinite(t->conf)) {
                t->active = 0U;
                raw_pred_cx[i] = 0.0f;
                raw_pred_cy[i] = 0.0f;
                pred_cx[i] = 0.0f;
                pred_cy[i] = 0.0f;
                continue;
            }
            raw_pred_cx[i] = t->cx + t->vx * dt;
            raw_pred_cy[i] = t->cy + t->vy * dt;
            pred_cx[i] = sb_clampf(raw_pred_cx[i], 0.0f, SB_FRAME_MAX_X);
            pred_cy[i] = sb_clampf(raw_pred_cy[i], 0.0f, SB_FRAME_MAX_Y);
            active_count++;
        } else {
            raw_pred_cx[i] = 0.0f;
            raw_pred_cy[i] = 0.0f;
            pred_cx[i] = 0.0f;
            pred_cy[i] = 0.0f;
        }
    }

    /* 2. Stage 1: match ALL active tracks against high_dets (CONFIRMED preferred on ties) */
    sb_match_stage(tr, pred_cx, pred_cy, high_list, high_count, dt,
                   0U, active_count == 1U && high_count == 1U, track_matched, high_matched);

    /* 3. Stage 2: match remaining unmatched CONFIRMED tracks against low_dets */
    sb_match_stage(tr, pred_cx, pred_cy, low_list, low_count, dt,
                   1U, 0U, track_matched, low_matched);

    /* 4. Coast or delete unmatched active tracks */
    for (i = 0U; i < SB_TRACKER_MAX_TRACKS; i++) {
        sb_track_t *t = &tr->tracks[i];
        uint8_t out_of_frame;

        if ((t->active == 0U) || (track_matched[i] != 0U)) {
            continue;
        }

        if (t->missed < 255U) {
            t->missed++;
        }
        if (t->age < 255U) {
            t->age++;
        }

        out_of_frame = (uint8_t)(!isfinite(raw_pred_cx[i]) || !isfinite(raw_pred_cy[i]) ||
                                 !isfinite(t->cx) || !isfinite(t->cy) ||
                                 !isfinite(t->w) || !isfinite(t->h) || !isfinite(t->conf) ||
                                 (raw_pred_cx[i] < 0.0f) || (raw_pred_cx[i] > SB_FRAME_MAX_X) ||
                                 (raw_pred_cy[i] < 0.0f) || (raw_pred_cy[i] > SB_FRAME_MAX_Y));

        if (t->confirmed == 0U) {
            if ((t->missed >= SB_TENTATIVE_MAX_MISSED) ||
                ((t->age >= SB_CONFIRM_MAX_AGE) && (t->hits < 2U)) ||
                (out_of_frame != 0U)) {
                t->active = 0U;
            } else {
                t->cx = pred_cx[i];
                t->cy = pred_cy[i];
                t->vx *= SB_COAST_VEL_DECAY;
                t->vy *= SB_COAST_VEL_DECAY;
                t->conf *= SB_COAST_CONF_DECAY;
            }
        } else {
            if ((t->missed > SB_COAST_MAX_MISSED) || (out_of_frame != 0U)) {
                t->active = 0U;
            } else {
                t->cx = pred_cx[i];
                t->cy = pred_cy[i];
                t->vx *= SB_COAST_VEL_DECAY;
                t->vy *= SB_COAST_VEL_DECAY;
                t->conf *= SB_COAST_CONF_DECAY;
            }
        }
    }

    /* 5. Spawn new tracks from unmatched high_dets only (highest confidence first) */
    if (high_count > 0U) {
        uint8_t spawn_order[UVC_AI_MAX_PREDICTIONS];
        uint8_t spawn_count = 0U;
        uint8_t j;

        for (i = 0U; i < high_count; i++) {
            if (high_matched[i] == 0U) {
                spawn_order[spawn_count++] = i;
            }
        }

        for (i = 1U; i < spawn_count; i++) {
            uint8_t key = spawn_order[i];
            float key_conf = high_list[key].conf;
            j = i;
            while ((j > 0U) && (high_list[spawn_order[j - 1U]].conf < key_conf)) {
                spawn_order[j] = spawn_order[j - 1U];
                j--;
            }
            spawn_order[j] = key;
        }

        for (i = 0U; i < spawn_count; i++) {
            const sb_det_t *det = &high_list[spawn_order[i]];
            uint8_t slot = sb_alloc_track_slot(tr);
            sb_track_t *t;

            if (slot >= SB_TRACKER_MAX_TRACKS) {
                break;
            }

            t = &tr->tracks[slot];
            memset(t, 0, sizeof(*t));
            t->active = 1U;
            float confirm_conf = (high_count == 1U && low_count == 0U && active_count == 0U) ?
                                 SB_SINGLE_CONFIRM_CONF : SB_INSTANT_CONFIRM_CONF;
            t->confirmed = det->conf >= confirm_conf;
            t->hits = 1U;
            t->age = 1U;
            t->missed = 0U;
            t->id = sb_next_track_id(tr);
            t->cx = sb_clampf(det->cx, 0.0f, SB_FRAME_MAX_X);
            t->cy = sb_clampf(det->cy, 0.0f, SB_FRAME_MAX_Y);
            t->w = det->w;
            t->h = det->h;
            t->vx = 0.0f;
            t->vy = 0.0f;
            t->conf = det->conf;
        }
    }

    /* 6. Emit active confirmed tracks sorted descending by confidence */
    for (i = 0U; i < SB_TRACKER_MAX_TRACKS; i++) {
        const sb_track_t *t = &tr->tracks[i];
        if ((t->active != 0U) && (t->confirmed != 0U) &&
            isfinite(t->cx) && isfinite(t->cy) &&
            isfinite(t->w) && isfinite(t->h) && isfinite(t->conf)) {
            emit_order[emit_count++] = i;
        }
    }

    for (i = 1U; i < emit_count; i++) {
        uint8_t key = emit_order[i];
        const sb_track_t *key_t = &tr->tracks[key];
        uint8_t j = i;
        while (j > 0U) {
            const sb_track_t *prev_t = &tr->tracks[emit_order[j - 1U]];
            if ((prev_t->conf < key_t->conf) ||
                ((prev_t->conf == key_t->conf) && (prev_t->id > key_t->id))) {
                emit_order[j] = emit_order[j - 1U];
                j--;
            } else {
                break;
            }
        }
        emit_order[j] = key;
    }

    if (emit_count > (uint8_t)UVC_AI_MAX_PREDICTIONS) {
        emit_count = (uint8_t)UVC_AI_MAX_PREDICTIONS;
    }

    out->valid = 1U;
    out->count = emit_count;
    out->inference_ms = (high_dets != NULL) ? high_dets->inference_ms : 0.0f;
    if (emit_count == 1U) {
        const sb_track_t *track = &tr->tracks[emit_order[0]];
        out->single_track_vx = (int16_t)track->vx;
        out->single_track_vy = (int16_t)track->vy;
    }

    for (i = 0U; i < emit_count; i++) {
        const sb_track_t *t = &tr->tracks[emit_order[i]];
        int16_t x1 = sb_round_clamp_i16(t->cx - 0.5f * t->w, 0, (int32_t)(UVC_AI_CAMERA_WIDTH - 1U));
        int16_t y1 = sb_round_clamp_i16(t->cy - 0.5f * t->h, 0, (int32_t)(UVC_AI_CAMERA_HEIGHT - 1U));
        int16_t x2 = sb_round_clamp_i16(t->cx + 0.5f * t->w, 0, (int32_t)(UVC_AI_CAMERA_WIDTH - 1U));
        int16_t y2 = sb_round_clamp_i16(t->cy + 0.5f * t->h, 0, (int32_t)(UVC_AI_CAMERA_HEIGHT - 1U));

        if (x2 < x1) {
            x2 = x1;
        }
        if (y2 < y1) {
            y2 = y1;
        }

        out->bbox_int16[(uint32_t)i * 4U + 0U] = x1;
        out->bbox_int16[(uint32_t)i * 4U + 1U] = y1;
        out->bbox_int16[(uint32_t)i * 4U + 2U] = x2;
        out->bbox_int16[(uint32_t)i * 4U + 3U] = y2;
        out->conf[i] = t->conf;
        out->class_id[i] = 0U;
        out->track_id[i] = t->id;
        sb_copy_class_label(out->class_string[i], UVC_AI_MAX_CLASS_LEN, "SteelBall");
    }
}

void sb_tracker_project_single(const uvc_ai_result_t *result, uint32_t age_ms, int16_t box[4])
{
    if (age_ms > SB_SINGLE_MAX_LEAD_MS) {
        age_ms = SB_SINGLE_MAX_LEAD_MS;
    }
    float seconds = 0.001f * (float)age_ms;
    float dx = sb_clampf(seconds * result->single_track_vx, -SB_SINGLE_MAX_LEAD_PX, SB_SINGLE_MAX_LEAD_PX);
    float dy = sb_clampf(seconds * result->single_track_vy, -SB_SINGLE_MAX_LEAD_PX, SB_SINGLE_MAX_LEAD_PX);
    box[0] = sb_round_clamp_i16(result->bbox_int16[0] + dx, 0, UVC_AI_CAMERA_WIDTH - 1U);
    box[1] = sb_round_clamp_i16(result->bbox_int16[1] + dy, 0, UVC_AI_CAMERA_HEIGHT - 1U);
    box[2] = sb_round_clamp_i16(result->bbox_int16[2] + dx, 0, UVC_AI_CAMERA_WIDTH - 1U);
    box[3] = sb_round_clamp_i16(result->bbox_int16[3] + dy, 0, UVC_AI_CAMERA_HEIGHT - 1U);
}
