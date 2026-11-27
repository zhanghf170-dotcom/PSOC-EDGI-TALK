#ifndef SB_TRACKER_H
#define SB_TRACKER_H

#include <stdint.h>
#include "uvc_ai.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SB_TRACKER_MAX_TRACKS 32U

typedef struct {
    uint8_t  active;
    uint8_t  confirmed;
    uint8_t  hits;
    uint8_t  age;
    uint8_t  missed;
    uint8_t  _pad;
    uint16_t id;
    float    cx;
    float    cy;
    float    w;
    float    h;
    float    vx;
    float    vy;
    float    conf;
} sb_track_t;

typedef struct {
    sb_track_t tracks[SB_TRACKER_MAX_TRACKS];
    uint32_t   last_ms;
    uint16_t   next_id;
    uint8_t    has_last_ts;
    uint8_t    _pad;
} sb_tracker_t;

void sb_tracker_init(sb_tracker_t *tr);
void sb_tracker_reset(sb_tracker_t *tr);
void sb_tracker_update(sb_tracker_t *tr,
                       const uvc_ai_result_t *high_dets,
                       const uvc_ai_result_t *low_dets,
                       uint32_t now_ms,
                       uvc_ai_result_t *out);
void sb_tracker_project_single(const uvc_ai_result_t *result, uint32_t age_ms, int16_t box[4]);

#ifdef __cplusplus
}
#endif

#endif /* SB_TRACKER_H */
