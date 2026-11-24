/*
 * Copyright (c) 2006-2025, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Alert Manager — async audio alert + SD persistent danger log + Wi-Fi notify
 */

#ifndef __ALERT_MANAGER_H__
#define __ALERT_MANAGER_H__

#include <rtthread.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ALERT_THRESHOLD         0.70f   /* Default danger threshold (50%~95% tunable) */

int      alert_manager_init(void);
int      alert_danger(float confidence, float temp, float humi);
void     alert_stop(void);
void     alert_set_cooldown_ms(uint32_t ms);
uint32_t alert_get_cooldown_ms(void);

uint32_t  alert_get_danger_count(void);
rt_tick_t alert_get_last_danger_tick(void);
void      alert_set_threshold(float thresh);
float     alert_get_threshold(void);
void      alert_set_volume(int vol);
int       alert_get_volume(void);
int       alert_test_sound(void);
int       alert_read_recent_logs(char *buf, size_t buf_size, int max_lines);
int       alert_clear_logs(void);

#ifdef __cplusplus
}
#endif

#endif
