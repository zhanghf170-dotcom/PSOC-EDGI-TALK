/*
 * Animated Cockpit Weather Screen (480x800, LVGL 9 + VG-Lite 60 FPS)
 *
 * Features:
 *   - 1-tap city selector (上海 / 北京 / 深圳) + live refresh button
 *   - Dynamic sky-gradient Hero Card with pure-vector animated weather scene:
 *     * Sunny   : glowing golden sun + breathing halo ring + floating light motes
 *     * Cloudy  : peeking sun + smoothly drifting fluffy multi-puff cloud
 *     * Overcast/Fog : drifting clouds + softly pulsing mist bands
 *     * Rain/Shower  : storm cloud + 6 staggered falling raindrops
 *     * Snow    : frost cloud + 6 softly floating snowflakes
 *     * Storm   : dark cloud + raindrops + flashing golden lightning pulse
 *   - Animated Temperature Comfort Scale Bar (-5°C .. 40°C)
 *   - 3 outdoor metric cards (体感温度 / 相对湿度+动态进度条 / 当前风速+风力等级)
 *   - Live onboard AHT20 indoor sensor comparison card + indoor/outdoor delta & cockpit advice
 *   - Animations automatically pause on screen leave (zero idle CPU cost)
 */

#include <stdio.h>
#include <math.h>
#include "ui_manager.h"
#include "ui_weather.h"

extern float g_aht_temp;
extern float g_aht_humi;

#define WX_PARTICLE_COUNT  6U
#define WX_CITY_COUNT      3U

typedef enum
{
    WX_SCENE_UNKNOWN = -1,
    WX_SCENE_SUNNY = 0,
    WX_SCENE_PARTLY_CLOUDY,
    WX_SCENE_CLOUDY_FOG,
    WX_SCENE_RAIN,
    WX_SCENE_SNOW,
    WX_SCENE_STORM
} wx_scene_t;

static ui_chrome_t g_chrome;

/* Top bar: city pills + refresh */
static lv_obj_t *g_city_btns[WX_CITY_COUNT];
static lv_obj_t *g_city_lbls[WX_CITY_COUNT];
static lv_obj_t *g_btn_sync;
static lv_obj_t *g_lbl_sync;

/* Hero sky card + vector scene */
static lv_obj_t *g_hero;
static lv_obj_t *g_lbl_loc;
static lv_obj_t *g_lbl_temp;
static lv_obj_t *g_lbl_unit;
static lv_obj_t *g_lbl_cond;
static lv_obj_t *g_lbl_tip;
static lv_obj_t *g_temp_dot;

static lv_obj_t *g_scene_box;
static lv_obj_t *g_sun_halo;
static lv_obj_t *g_sun_core;
static lv_obj_t *g_cloud_grp;
static lv_obj_t *g_cloud_puffs[3];
static lv_obj_t *g_bolt_bar;
static lv_obj_t *g_particles[WX_PARTICLE_COUNT];

/* 3 Outdoor Metric Tiles */
static lv_obj_t *g_lbl_app_val;
static lv_obj_t *g_lbl_app_tag;
static lv_obj_t *g_lbl_humi_val;
static lv_obj_t *g_bar_humi;
static lv_obj_t *g_lbl_wind_val;
static lv_obj_t *g_lbl_wind_tag;

/* Indoor AHT20 vs Outdoor Comparison Card */
static lv_obj_t *g_indoor_dot;
static lv_obj_t *g_lbl_in_temp;
static lv_obj_t *g_lbl_in_humi;
static lv_obj_t *g_lbl_delta;
static lv_obj_t *g_lbl_cabin_tip;

/* Source & Observation Footer */
static lv_obj_t *g_src_dot;
static lv_obj_t *g_lbl_status;
static lv_obj_t *g_lbl_time;

static wx_scene_t g_active_scene = WX_SCENE_UNKNOWN;
static bool       g_anim_running = false;
static uint32_t   g_sync_pulse_until = 0U;

const char *ui_weather_city(int city)
{
    static const char *const cities[] = {"上海", "北京", "深圳", "自定义地点"};
    return city >= 0 && city < (int)(sizeof(cities) / sizeof(cities[0])) ? cities[city] : cities[0];
}

const char *ui_weather_condition(int code)
{
    if (code < 0) return "等待天气数据";
    if (code == 0) return "晴";
    if (code <= 2) return "多云";
    if (code == 3) return "阴";
    if (code <= 48) return "雾";
    if (code <= 57) return "毛毛雨";
    if (code <= 67) return "雨";
    if (code <= 77) return "雪";
    if (code <= 82) return "阵雨";
    if (code <= 86) return "阵雪";
    return "雷暴";
}

bool ui_weather_is_fresh(const watch_info_t *info, time_t now)
{
    return info->weather_code >= 0 && now >= info->weather_epoch &&
           now - info->weather_epoch <= WATCH_WEATHER_MAX_AGE;
}

static wx_scene_t classify_scene(int code)
{
    if (code <= 0)  return WX_SCENE_SUNNY;
    if (code <= 2)  return WX_SCENE_PARTLY_CLOUDY;
    if (code <= 48) return WX_SCENE_CLOUDY_FOG;
    if (code <= 67) return WX_SCENE_RAIN;
    if (code <= 77) return WX_SCENE_SNOW;
    if (code <= 82) return WX_SCENE_RAIN;
    if (code <= 86) return WX_SCENE_SNOW;
    return WX_SCENE_STORM;
}

static const char *driving_tip_for_code(int code)
{
    if (code < 0)   return "连接网络后自动获取实时气象";
    if (code == 0)  return "晴朗通透 · 适宜出行驾驶";
    if (code <= 2)  return "云量适中 · 道路视线良好";
    if (code == 3)  return "天色偏阴 · 注意行车视线";
    if (code <= 48) return "雾霾偏重 · 建议开启雾灯减速";
    if (code <= 67 || (code >= 80 && code <= 82))
                    return "路面湿滑 · 请放慢车速保持车距";
    if (code <= 77 || (code >= 85 && code <= 86))
                    return "雨雪降温 · 注意防滑谨慎驾驶";
    return "雷雨天气 · 建议避险谨慎出行";
}

static const char *comfort_tag(int temp10)
{
    if (temp10 < 50)  return "偏冷保暖";
    if (temp10 < 150) return "微凉舒适";
    if (temp10 < 260) return "体感舒适";
    if (temp10 < 320) return "偏暖微热";
    return "高温防暑";
}

static const char *wind_level_tag(int wind10)
{
    if (wind10 < 0)   return "实时测风";
    if (wind10 < 60)  return "1级软风";
    if (wind10 < 120) return "2级轻风";
    if (wind10 < 200) return "3级微风";
    if (wind10 < 290) return "4级和风";
    if (wind10 < 390) return "5级清风";
    return "6级+强风";
}

/* ---------------- LVGL Animation Exec Callbacks ---------------- */

static void anim_set_x_cb(void *var, int32_t v)      { lv_obj_set_x((lv_obj_t *)var, v); }
static void anim_set_y_cb(void *var, int32_t v)      { lv_obj_set_y((lv_obj_t *)var, v); }
static void anim_set_opa_cb(void *var, int32_t v)    { lv_obj_set_style_bg_opa((lv_obj_t *)var, (lv_opa_t)v, 0); }

static void anim_sun_halo_cb(void *var, int32_t v)
{
    lv_obj_t *o = (lv_obj_t *)var;
    lv_obj_set_size(o, v, v);
    lv_obj_set_pos(o, 88 - v / 2, 46 - v / 2);
    int32_t opa = 175 - ((v - 66) * 4);
    if (opa < 35) opa = 35;
    if (opa > 180) opa = 180;
    lv_obj_set_style_border_opa(o, (lv_opa_t)opa, 0);
}

static void stop_weather_anims(void)
{
    g_anim_running = false;
    if (g_sun_halo)   lv_anim_delete(g_sun_halo, NULL);
    if (g_cloud_grp)  lv_anim_delete(g_cloud_grp, NULL);
    if (g_bolt_bar)   lv_anim_delete(g_bolt_bar, NULL);
    if (g_indoor_dot) lv_anim_delete(g_indoor_dot, NULL);
    for (unsigned i = 0; i < WX_PARTICLE_COUNT; i++)
    {
        if (g_particles[i]) lv_anim_delete(g_particles[i], NULL);
    }
}

static void apply_scene_visuals(wx_scene_t scene, bool start_anims)
{
    g_active_scene = scene;

    /* 1. Dynamic sky card gradient */
    lv_color_t c_top, c_bot, c_border;
    switch (scene)
    {
    case WX_SCENE_SUNNY:
        c_top = lv_color_hex(0x0E315E); c_bot = lv_color_hex(0x1D4A78); c_border = lv_color_hex(0x3878B8);
        break;
    case WX_SCENE_PARTLY_CLOUDY:
        c_top = lv_color_hex(0x132C4E); c_bot = lv_color_hex(0x213A5E); c_border = lv_color_hex(0x365B8C);
        break;
    case WX_SCENE_CLOUDY_FOG:
        c_top = lv_color_hex(0x1A2538); c_bot = lv_color_hex(0x2B3B52); c_border = lv_color_hex(0x475A75);
        break;
    case WX_SCENE_RAIN:
        c_top = lv_color_hex(0x0D1E36); c_bot = lv_color_hex(0x193252); c_border = lv_color_hex(0x2E5C8A);
        break;
    case WX_SCENE_SNOW:
        c_top = lv_color_hex(0x122845); c_bot = lv_color_hex(0x24466E); c_border = lv_color_hex(0x5B8EC2);
        break;
    default: /* STORM */
        c_top = lv_color_hex(0x1A1638); c_bot = lv_color_hex(0x2D2356); c_border = lv_color_hex(0x6D55B8);
        break;
    }
    lv_obj_set_style_bg_color(g_hero, c_top, 0);
    lv_obj_set_style_bg_grad_color(g_hero, c_bot, 0);
    lv_obj_set_style_border_color(g_hero, c_border, 0);

    /* 2. Toggle Sun / Cloud / Lightning visibility & styling */
    bool show_sun   = (scene == WX_SCENE_SUNNY || scene == WX_SCENE_PARTLY_CLOUDY);
    bool show_cloud = (scene != WX_SCENE_SUNNY);
    bool show_bolt  = (scene == WX_SCENE_STORM);

    if (show_sun)
    {
        lv_obj_remove_flag(g_sun_core, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(g_sun_halo, LV_OBJ_FLAG_HIDDEN);
        int32_t sx = (scene == WX_SCENE_SUNNY) ? 61 : 74;
        int32_t sy = (scene == WX_SCENE_SUNNY) ? 19 : 12;
        lv_obj_set_pos(g_sun_core, sx, sy);
    }
    else
    {
        lv_obj_add_flag(g_sun_core, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(g_sun_halo, LV_OBJ_FLAG_HIDDEN);
    }

    if (show_cloud)
    {
        lv_obj_remove_flag(g_cloud_grp, LV_OBJ_FLAG_HIDDEN);
        lv_color_t cc = (scene == WX_SCENE_RAIN || scene == WX_SCENE_STORM)
                            ? lv_color_hex(0x64748B)
                            : (scene == WX_SCENE_SNOW ? lv_color_hex(0xCBD5E1) : lv_color_hex(0xE2E8F0));
        for (int i = 0; i < 3; i++)
            lv_obj_set_style_bg_color(g_cloud_puffs[i], cc, 0);
    }
    else
    {
        lv_obj_add_flag(g_cloud_grp, LV_OBJ_FLAG_HIDDEN);
    }

    if (show_bolt)
        lv_obj_remove_flag(g_bolt_bar, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(g_bolt_bar, LV_OBJ_FLAG_HIDDEN);

    /* 3. Configure particles for this weather scene */
    static const int32_t px_rain[WX_PARTICLE_COUNT]  = { 30, 52, 74, 96, 118, 63 };
    static const int32_t px_sun[WX_PARTICLE_COUNT]   = { 24, 134, 34, 128, 72, 108 };
    static const int32_t py_sun[WX_PARTICLE_COUNT]   = { 26, 22, 92, 86, 112, 106 };

    for (unsigned i = 0; i < WX_PARTICLE_COUNT; i++)
    {
        lv_obj_t *p = g_particles[i];
        lv_anim_delete(p, NULL);

        if (scene == WX_SCENE_RAIN || scene == WX_SCENE_STORM)
        {
            lv_obj_remove_flag(p, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_size(p, 4, 15);
            lv_obj_set_style_radius(p, 2, 0);
            lv_obj_set_style_bg_color(p, lv_color_hex(0x38BDF8), 0);
            lv_obj_set_style_bg_opa(p, LV_OPA_80, 0);
            lv_obj_set_pos(p, px_rain[i], 88);
        }
        else if (scene == WX_SCENE_SNOW)
        {
            lv_obj_remove_flag(p, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_size(p, 7, 7);
            lv_obj_set_style_radius(p, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(p, lv_color_hex(0xF8FAFC), 0);
            lv_obj_set_style_bg_opa(p, LV_OPA_90, 0);
            lv_obj_set_pos(p, px_rain[i], 86);
        }
        else if (scene == WX_SCENE_CLOUDY_FOG)
        {
            if (i < 3U)
            {
                lv_obj_remove_flag(p, LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_size(p, 64 + (int32_t)i * 18, 6);
                lv_obj_set_style_radius(p, 3, 0);
                lv_obj_set_style_bg_color(p, lv_color_hex(0x94A3B8), 0);
                lv_obj_set_style_bg_opa(p, LV_OPA_50, 0);
                lv_obj_set_pos(p, 24 + (int32_t)i * 10, 96 + (int32_t)i * 15);
            }
            else
            {
                lv_obj_add_flag(p, LV_OBJ_FLAG_HIDDEN);
            }
        }
        else /* SUNNY or PARTLY_CLOUDY: subtle ambient golden light motes */
        {
            if (i < 4U)
            {
                lv_obj_remove_flag(p, LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_size(p, 6, 6);
                lv_obj_set_style_radius(p, LV_RADIUS_CIRCLE, 0);
                lv_obj_set_style_bg_color(p, lv_color_hex(0xFDE68A), 0);
                lv_obj_set_style_bg_opa(p, LV_OPA_60, 0);
                lv_obj_set_pos(p, px_sun[i], py_sun[i]);
            }
            else
            {
                lv_obj_add_flag(p, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }

    if (!start_anims) return;

    /* 4. Launch smooth 60 FPS LVGL animations */
    stop_weather_anims();
    g_anim_running = true;

    if (show_sun)
    {
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, g_sun_halo);
        lv_anim_set_values(&a, 68, 92);
        lv_anim_set_duration(&a, 1800);
        lv_anim_set_playback_duration(&a, 1800);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
        lv_anim_set_exec_cb(&a, anim_sun_halo_cb);
        lv_anim_start(&a);
    }

    if (show_cloud)
    {
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, g_cloud_grp);
        lv_anim_set_values(&a, 18, 34);
        lv_anim_set_duration(&a, 2400);
        lv_anim_set_playback_duration(&a, 2400);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
        lv_anim_set_exec_cb(&a, anim_set_x_cb);
        lv_anim_start(&a);
    }

    if (show_bolt)
    {
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, g_bolt_bar);
        lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
        lv_anim_set_duration(&a, 160);
        lv_anim_set_playback_duration(&a, 220);
        lv_anim_set_repeat_delay(&a, 1900);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_exec_cb(&a, anim_set_opa_cb);
        lv_anim_start(&a);
    }

    for (unsigned i = 0; i < WX_PARTICLE_COUNT; i++)
    {
        if (lv_obj_has_flag(g_particles[i], LV_OBJ_FLAG_HIDDEN)) continue;
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, g_particles[i]);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);

        if (scene == WX_SCENE_RAIN || scene == WX_SCENE_STORM)
        {
            lv_anim_set_values(&a, 86, 138);
            lv_anim_set_duration(&a, 620 + (i % 3U) * 110U);
            lv_anim_set_delay(&a, i * 115U);
            lv_anim_set_exec_cb(&a, anim_set_y_cb);
            lv_anim_start(&a);
        }
        else if (scene == WX_SCENE_SNOW)
        {
            lv_anim_set_values(&a, 84, 138);
            lv_anim_set_duration(&a, 1700 + (i % 3U) * 320U);
            lv_anim_set_delay(&a, i * 260U);
            lv_anim_set_exec_cb(&a, anim_set_y_cb);
            lv_anim_start(&a);
        }
        else if (scene == WX_SCENE_CLOUDY_FOG)
        {
            int32_t base_x = 20 + (int32_t)i * 10;
            lv_anim_set_values(&a, base_x, base_x + 18);
            lv_anim_set_duration(&a, 2100 + i * 350U);
            lv_anim_set_playback_duration(&a, 2100 + i * 350U);
            lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
            lv_anim_set_exec_cb(&a, anim_set_x_cb);
            lv_anim_start(&a);
        }
        else /* Sunny / partly cloudy golden motes */
        {
            lv_anim_set_values(&a, 30, 210);
            lv_anim_set_duration(&a, 1100 + i * 240U);
            lv_anim_set_playback_duration(&a, 1100 + i * 240U);
            lv_anim_set_delay(&a, i * 220U);
            lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
            lv_anim_set_exec_cb(&a, anim_set_opa_cb);
            lv_anim_start(&a);
        }
    }

    /* Live green pulse on onboard AHT20 indoor sensor dot */
    if (g_indoor_dot)
    {
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, g_indoor_dot);
        lv_anim_set_values(&a, LV_OPA_40, LV_OPA_COVER);
        lv_anim_set_duration(&a, 1000);
        lv_anim_set_playback_duration(&a, 1000);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_exec_cb(&a, anim_set_opa_cb);
        lv_anim_start(&a);
    }
}

/* ---------------- Event Callbacks ---------------- */

static void city_btn_cb(lv_event_t *e)
{
    int city_id = (int)(intptr_t)lv_event_get_user_data(e);
    g_sync_pulse_until = lv_tick_get() + 2500U;
    watch_info_set_city(city_id);
    ui_screen_weather_update();
}

static void sync_btn_cb(lv_event_t *e)
{
    (void)e;
    g_sync_pulse_until = lv_tick_get() + 2500U;
    watch_info_trigger_sync();
    ui_screen_weather_update();
}

/* ---------------- Widget Helpers ---------------- */

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *txt)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt ? txt : "");
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    return l;
}

static lv_obj_t *make_card(lv_obj_t *parent, int32_t w, int32_t h)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_set_size(c, w, h);
    lv_obj_add_style(c, &g_st_card, 0);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

static lv_obj_t *make_metric_tile(lv_obj_t *parent, const char *title, lv_color_t accent,
                                  lv_obj_t **val_out, lv_obj_t **sub_out, lv_obj_t **bar_out)
{
    const lv_font_t *f_small = ui_font_zh_small();
    const lv_font_t *f_full  = ui_font_zh_full();

    lv_obj_t *tile = make_card(parent, 146, 106);
    lv_obj_set_style_pad_all(tile, 10, 0);

    lv_obj_t *hdr = lv_obj_create(tile);
    lv_obj_remove_style_all(hdr);
    lv_obj_set_size(hdr, 126, 22);
    lv_obj_align(hdr, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_remove_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(hdr, 6, 0);

    lv_obj_t *dot = lv_obj_create(hdr);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 8, 8);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, accent, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);

    (void)make_label(hdr, f_small, UI_COL_MUTED, title);

    lv_obj_t *v = make_label(tile, f_full, UI_COL_TEXT, "--");
    lv_obj_align(v, LV_ALIGN_LEFT_MID, 0, 2);
    *val_out = v;

    if (bar_out != NULL)
    {
        lv_obj_t *bar = lv_bar_create(tile);
        lv_obj_set_size(bar, 122, 8);
        lv_obj_align(bar, LV_ALIGN_BOTTOM_LEFT, 0, -4);
        lv_bar_set_range(bar, 0, 100);
        lv_bar_set_value(bar, 0, LV_ANIM_OFF);
        lv_obj_set_style_radius(bar, 4, LV_PART_MAIN);
        lv_obj_set_style_bg_color(bar, lv_color_hex(0x1E293B), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(bar, 4, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(bar, accent, LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
        *bar_out = bar;
    }
    else if (sub_out != NULL)
    {
        lv_obj_t *s = make_label(tile, f_small, accent, "--");
        lv_obj_align(s, LV_ALIGN_BOTTOM_LEFT, 0, 0);
        *sub_out = s;
    }

    return tile;
}

/* ---------------- Screen Creation ---------------- */

lv_obj_t *ui_screen_weather_create(void)
{
    const lv_font_t *f_small = ui_font_zh_small();
    const lv_font_t *f_full  = ui_font_zh_full();

    lv_obj_t *screen = lv_obj_create(NULL);
    ui_create_chrome(screen, "天气实况", UI_SCR_WEATHER, &g_chrome);

    lv_obj_t *body = g_chrome.body;
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(body, 10, 0);
    lv_obj_set_style_pad_row(body, 10, 0);

    /* 1. Top City Selector + Refresh Bar (460 x 38) */
    lv_obj_t *top_row = lv_obj_create(body);
    lv_obj_remove_style_all(top_row);
    lv_obj_set_size(top_row, 460, 38);
    lv_obj_remove_flag(top_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(top_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(top_row, 8, 0);

    static const char *const city_names[WX_CITY_COUNT] = {"上海", "北京", "深圳"};
    for (unsigned i = 0; i < WX_CITY_COUNT; i++)
    {
        g_city_btns[i] = lv_button_create(top_row);
        lv_obj_add_style(g_city_btns[i], &g_st_btn_ghost, 0);
        lv_obj_set_size(g_city_btns[i], 102, 36);
        lv_obj_set_style_radius(g_city_btns[i], 18, 0);
        lv_obj_add_event_cb(g_city_btns[i], city_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        g_city_lbls[i] = make_label(g_city_btns[i], f_small, UI_COL_MUTED, city_names[i]);
        lv_obj_center(g_city_lbls[i]);
    }

    lv_obj_t *spacer = lv_obj_create(top_row);
    lv_obj_remove_style_all(spacer);
    lv_obj_set_flex_grow(spacer, 1);
    lv_obj_set_height(spacer, 1);

    g_btn_sync = lv_button_create(top_row);
    lv_obj_add_style(g_btn_sync, &g_st_btn_ghost, 0);
    lv_obj_set_size(g_btn_sync, 114, 36);
    lv_obj_set_style_radius(g_btn_sync, 18, 0);
    lv_obj_set_style_border_color(g_btn_sync, UI_COL_CYAN, 0);
    lv_obj_add_event_cb(g_btn_sync, sync_btn_cb, LV_EVENT_CLICKED, NULL);
    g_lbl_sync = make_label(g_btn_sync, f_small, UI_COL_CYAN, LV_SYMBOL_REFRESH " 刷新");
    lv_obj_center(g_lbl_sync);

    /* 2. Animated Dynamic Sky Hero Card (460 x 284) */
    g_hero = make_card(body, 460, 284);
    lv_obj_set_style_radius(g_hero, 22, 0);
    lv_obj_set_style_bg_color(g_hero, lv_color_hex(0x0E315E), 0);
    lv_obj_set_style_bg_grad_color(g_hero, lv_color_hex(0x1D4A78), 0);
    lv_obj_set_style_bg_grad_dir(g_hero, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_border_width(g_hero, 2, 0);
    lv_obj_set_style_pad_all(g_hero, 18, 0);

    /* Left typography column */
    g_lbl_loc = make_label(g_hero, f_small, UI_COL_CYAN, LV_SYMBOL_GPS " 上海 · 室外实况");
    lv_obj_align(g_lbl_loc, LV_ALIGN_TOP_LEFT, 0, 2);

    g_lbl_temp = make_label(g_hero, &lv_font_montserrat_48, UI_COL_TEXT, "--");
    lv_obj_align(g_lbl_temp, LV_ALIGN_TOP_LEFT, 0, 44);

    g_lbl_unit = make_label(g_hero, f_full, UI_COL_MUTED, "℃");
    lv_obj_align_to(g_lbl_unit, g_lbl_temp, LV_ALIGN_OUT_RIGHT_TOP, 4, 8);

    g_lbl_cond = make_label(g_hero, f_full, UI_COL_TEXT, "等待天气数据");
    lv_obj_align(g_lbl_cond, LV_ALIGN_TOP_LEFT, 2, 118);

    g_lbl_tip = make_label(g_hero, f_small, lv_color_hex(0xCBD5E1), "连接网络后自动获取实时气象");
    lv_obj_align(g_lbl_tip, LV_ALIGN_TOP_LEFT, 2, 154);

    /* Right pure-vector animated scene stage (166 x 154) */
    g_scene_box = lv_obj_create(g_hero);
    lv_obj_remove_style_all(g_scene_box);
    lv_obj_set_size(g_scene_box, 166, 154);
    lv_obj_align(g_scene_box, LV_ALIGN_TOP_RIGHT, 6, -4);
    lv_obj_remove_flag(g_scene_box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    /* Sun halo ring */
    g_sun_halo = lv_obj_create(g_scene_box);
    lv_obj_remove_style_all(g_sun_halo);
    lv_obj_set_size(g_sun_halo, 78, 78);
    lv_obj_set_pos(g_sun_halo, 49, 7);
    lv_obj_set_style_radius(g_sun_halo, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(g_sun_halo, 5, 0);
    lv_obj_set_style_border_color(g_sun_halo, lv_color_hex(0xFDE047), 0);
    lv_obj_set_style_border_opa(g_sun_halo, LV_OPA_60, 0);

    /* Sun core */
    g_sun_core = lv_obj_create(g_scene_box);
    lv_obj_remove_style_all(g_sun_core);
    lv_obj_set_size(g_sun_core, 54, 54);
    lv_obj_set_pos(g_sun_core, 61, 19);
    lv_obj_set_style_radius(g_sun_core, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(g_sun_core, lv_color_hex(0xFACC15), 0);
    lv_obj_set_style_bg_grad_color(g_sun_core, lv_color_hex(0xF97316), 0);
    lv_obj_set_style_bg_grad_dir(g_sun_core, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(g_sun_core, LV_OPA_COVER, 0);

    /* Layered 3-puff fluffy cloud group */
    g_cloud_grp = lv_obj_create(g_scene_box);
    lv_obj_remove_style_all(g_cloud_grp);
    lv_obj_set_size(g_cloud_grp, 120, 62);
    lv_obj_set_pos(g_cloud_grp, 24, 34);
    lv_obj_remove_flag(g_cloud_grp, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    /* Cloud puffs: left circle, center-top dome, wide base pill */
    static const int32_t c_rect[3][4] = {
        { 12, 18, 42, 42 },
        { 36,  4, 54, 54 },
        {  6, 28, 108, 32 }
    };
    for (int i = 0; i < 3; i++)
    {
        g_cloud_puffs[i] = lv_obj_create(g_cloud_grp);
        lv_obj_remove_style_all(g_cloud_puffs[i]);
        lv_obj_set_pos(g_cloud_puffs[i], c_rect[i][0], c_rect[i][1]);
        lv_obj_set_size(g_cloud_puffs[i], c_rect[i][2], c_rect[i][3]);
        lv_obj_set_style_radius(g_cloud_puffs[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(g_cloud_puffs[i], lv_color_hex(0xE2E8F0), 0);
        lv_obj_set_style_bg_opa(g_cloud_puffs[i], LV_OPA_COVER, 0);
    }

    /* Lightning flash bar under cloud (storm scene only) */
    g_bolt_bar = lv_obj_create(g_scene_box);
    lv_obj_remove_style_all(g_bolt_bar);
    lv_obj_set_size(g_bolt_bar, 10, 34);
    lv_obj_set_pos(g_bolt_bar, 78, 88);
    lv_obj_set_style_radius(g_bolt_bar, 3, 0);
    lv_obj_set_style_bg_color(g_bolt_bar, lv_color_hex(0xFDE047), 0);
    lv_obj_set_style_bg_opa(g_bolt_bar, LV_OPA_TRANSP, 0);

    /* 6 Animated weather particles (rain / snow / fog / sun motes) */
    for (unsigned i = 0; i < WX_PARTICLE_COUNT; i++)
    {
        g_particles[i] = lv_obj_create(g_scene_box);
        lv_obj_remove_style_all(g_particles[i]);
        lv_obj_add_flag(g_particles[i], LV_OBJ_FLAG_HIDDEN);
    }

    /* Bottom Temperature Comfort Scale Bar inside Hero Card (-5°C .. 40°C) */
    lv_obj_t *scale_lbl_l = make_label(g_hero, f_small, lv_color_hex(0x93C5FD), "-5°");
    lv_obj_align(scale_lbl_l, LV_ALIGN_BOTTOM_LEFT, 0, -24);
    lv_obj_t *scale_lbl_m = make_label(g_hero, f_small, lv_color_hex(0x6EE7B7), "温度舒适刻度");
    lv_obj_align(scale_lbl_m, LV_ALIGN_BOTTOM_MID, 0, -24);
    lv_obj_t *scale_lbl_r = make_label(g_hero, f_small, lv_color_hex(0xFCA5A5), "40°");
    lv_obj_align(scale_lbl_r, LV_ALIGN_BOTTOM_RIGHT, 0, -24);

    lv_obj_t *scale_track = lv_obj_create(g_hero);
    lv_obj_remove_style_all(scale_track);
    lv_obj_set_size(scale_track, 420, 10);
    lv_obj_align(scale_track, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_set_flex_flow(scale_track, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(scale_track, 3, 0);

    static const uint32_t seg_cols[4] = { 0x38BDF8, 0x10B981, 0xF59E0B, 0xEF4444 };
    for (int i = 0; i < 4; i++)
    {
        lv_obj_t *seg = lv_obj_create(scale_track);
        lv_obj_remove_style_all(seg);
        lv_obj_set_height(seg, 10);
        lv_obj_set_flex_grow(seg, 1);
        lv_obj_set_style_radius(seg, 5, 0);
        lv_obj_set_style_bg_color(seg, lv_color_hex(seg_cols[i]), 0);
        lv_obj_set_style_bg_opa(seg, LV_OPA_80, 0);
    }

    g_temp_dot = lv_obj_create(g_hero);
    lv_obj_remove_style_all(g_temp_dot);
    lv_obj_set_size(g_temp_dot, 16, 16);
    lv_obj_set_style_radius(g_temp_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(g_temp_dot, UI_COL_TEXT, 0);
    lv_obj_set_style_bg_opa(g_temp_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_temp_dot, 3, 0);
    lv_obj_set_style_border_color(g_temp_dot, UI_COL_CYAN, 0);
    lv_obj_set_pos(g_temp_dot, 180, 229);

    /* 3. 3-Column Outdoor Metrics Row (460 x 106) */
    lv_obj_t *metrics = lv_obj_create(body);
    lv_obj_remove_style_all(metrics);
    lv_obj_set_size(metrics, 460, 106);
    lv_obj_remove_flag(metrics, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(metrics, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(metrics, 11, 0);

    make_metric_tile(metrics, "体感温度", UI_COL_AMBER, &g_lbl_app_val, &g_lbl_app_tag, NULL);
    make_metric_tile(metrics, "相对湿度", UI_COL_CYAN,  &g_lbl_humi_val, NULL, &g_bar_humi);
    make_metric_tile(metrics, "当前风速", UI_COL_GREEN, &g_lbl_wind_val, &g_lbl_wind_tag, NULL);

    /* 4. Indoor AHT20 vs Outdoor Comparison Card (460 x 116) */
    lv_obj_t *cabin = make_card(body, 460, 116);
    lv_obj_set_style_pad_hor(cabin, 14, 0);
    lv_obj_set_style_pad_ver(cabin, 10, 0);

    g_indoor_dot = lv_obj_create(cabin);
    lv_obj_remove_style_all(g_indoor_dot);
    lv_obj_set_size(g_indoor_dot, 10, 10);
    lv_obj_set_style_radius(g_indoor_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(g_indoor_dot, UI_COL_GREEN, 0);
    lv_obj_set_style_bg_opa(g_indoor_dot, LV_OPA_COVER, 0);
    lv_obj_align(g_indoor_dot, LV_ALIGN_TOP_LEFT, 0, 5);

    lv_obj_t *in_title = make_label(cabin, f_small, UI_COL_GREEN, "车内实测 (AHT20)");
    lv_obj_align(in_title, LV_ALIGN_TOP_LEFT, 16, 0);

    g_lbl_in_temp = make_label(cabin, f_full, UI_COL_TEXT, "--.- ℃");
    lv_obj_align(g_lbl_in_temp, LV_ALIGN_LEFT_MID, 0, 6);

    g_lbl_in_humi = make_label(cabin, f_small, UI_COL_MUTED, "湿度 --%");
    lv_obj_align(g_lbl_in_humi, LV_ALIGN_BOTTOM_LEFT, 0, -2);

    /* Subtle vertical divider */
    lv_obj_t *vdiv = lv_obj_create(cabin);
    lv_obj_remove_style_all(vdiv);
    lv_obj_set_size(vdiv, 2, 82);
    lv_obj_set_style_bg_color(vdiv, UI_COL_BORDER, 0);
    lv_obj_set_style_bg_opa(vdiv, LV_OPA_COVER, 0);
    lv_obj_align(vdiv, LV_ALIGN_CENTER, -18, 0);

    lv_obj_t *cmp_title = make_label(cabin, f_small, UI_COL_GEMINI_PURPLE, "内外环境对比");
    lv_obj_align(cmp_title, LV_ALIGN_TOP_LEFT, 212, 0);

    g_lbl_delta = make_label(cabin, f_full, UI_COL_TEXT, "温差计算中");
    lv_obj_align(g_lbl_delta, LV_ALIGN_LEFT_MID, 212, 6);

    g_lbl_cabin_tip = make_label(cabin, f_small, UI_COL_MUTED, "等待温湿度数据");
    lv_obj_align(g_lbl_cabin_tip, LV_ALIGN_BOTTOM_LEFT, 212, -2);

    /* 5. Bottom Source & Observation Footer (460 x 70) */
    lv_obj_t *footer = make_card(body, 460, 70);
    lv_obj_set_style_pad_hor(footer, 14, 0);
    lv_obj_set_style_pad_ver(footer, 10, 0);

    g_src_dot = lv_obj_create(footer);
    lv_obj_remove_style_all(g_src_dot);
    lv_obj_set_size(g_src_dot, 10, 10);
    lv_obj_set_style_radius(g_src_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(g_src_dot, UI_COL_CYAN, 0);
    lv_obj_set_style_bg_opa(g_src_dot, LV_OPA_COVER, 0);
    lv_obj_align(g_src_dot, LV_ALIGN_TOP_LEFT, 0, 6);

    g_lbl_status = make_label(footer, f_small, UI_COL_CYAN, "等待同步");
    lv_obj_align(g_lbl_status, LV_ALIGN_TOP_LEFT, 16, 0);

    g_lbl_time = make_label(footer, f_small, UI_COL_MUTED, "点击上方城市或刷新按钮获取实时天气");
    lv_obj_align(g_lbl_time, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    apply_scene_visuals(WX_SCENE_SUNNY, false);
    ui_screen_weather_update();
    return screen;
}

void ui_screen_weather_on_enter(void)
{
    watch_info_t info;
    watch_info_get(&info);
    apply_scene_visuals(classify_scene(info.weather_code), true);
    ui_screen_weather_update();
}

void ui_screen_weather_on_leave(void)
{
    stop_weather_anims();
}

void ui_screen_weather_update(void)
{
    watch_info_t info;
    char text[96];

    ui_update_chrome(&g_chrome);
    watch_info_get(&info);
    time_t now = time(NULL);

    /* 1. Highlight active city button */
    int sel_city = watch_info_get_selected_city();
    for (unsigned i = 0; i < WX_CITY_COUNT; i++)
    {
        bool active = ((int)i == sel_city);
        lv_obj_set_style_bg_color(g_city_btns[i], active ? lv_color_hex(0x163459) : UI_COL_CARD, 0);
        lv_obj_set_style_border_color(g_city_btns[i], active ? UI_COL_CYAN : UI_COL_BORDER, 0);
        lv_obj_set_style_text_color(g_city_lbls[i], active ? UI_COL_CYAN : UI_COL_MUTED, 0);
    }

    bool syncing = (g_sync_pulse_until != 0U && (int32_t)(g_sync_pulse_until - lv_tick_get()) > 0);
    ui_set_label_text(g_lbl_sync, syncing ? LV_SYMBOL_REFRESH " 同步中" : LV_SYMBOL_REFRESH " 刷新");

    /* 2. Switch weather sky + vector scene when WMO weather code changes */
    wx_scene_t wanted_scene = classify_scene(info.weather_code);
    if (wanted_scene != g_active_scene)
    {
        apply_scene_visuals(wanted_scene, g_anim_running);
    }

    /* 3. Update Hero Card */
    snprintf(text, sizeof(text), LV_SYMBOL_GPS " %s · 室外实况", ui_weather_city(info.city));
    ui_set_label_text(g_lbl_loc, text);
    ui_set_label_text(g_lbl_cond, ui_weather_condition(info.weather_code));
    ui_set_label_text(g_lbl_tip, driving_tip_for_code(info.weather_code));

    /* 4. Update Indoor AHT20 Card */
    bool aht_ok = (!isnan(g_aht_temp) && g_aht_temp > -40.0f && g_aht_temp < 85.0f &&
                   !isnan(g_aht_humi) && g_aht_humi >= 0.0f && g_aht_humi <= 100.0f);
    if (aht_ok)
    {
        snprintf(text, sizeof(text), "%.1f ℃", (double)g_aht_temp);
        ui_set_label_text(g_lbl_in_temp, text);
        snprintf(text, sizeof(text), "相对湿度 %.0f%%", (double)g_aht_humi);
        ui_set_label_text(g_lbl_in_humi, text);
    }
    else
    {
        ui_set_label_text(g_lbl_in_temp, "--.- ℃");
        ui_set_label_text(g_lbl_in_humi, "传感器采样中");
    }

    if (info.weather_code < 0)
    {
        ui_set_label_text(g_lbl_temp, "--");
        lv_obj_align_to(g_lbl_unit, g_lbl_temp, LV_ALIGN_OUT_RIGHT_TOP, 4, 8);
        ui_set_label_text(g_lbl_app_val, "--");
        ui_set_label_text(g_lbl_app_tag, "等待同步");
        ui_set_label_text(g_lbl_humi_val, "--");
        lv_bar_set_value(g_bar_humi, 0, LV_ANIM_OFF);
        ui_set_label_text(g_lbl_wind_val, "--");
        ui_set_label_text(g_lbl_wind_tag, "等待同步");
        ui_set_label_text(g_lbl_delta, "等待室外数据");
        ui_set_label_text(g_lbl_cabin_tip, aht_ok ? "车内传感器运行正常" : "等待传感器数据");
        lv_obj_set_style_bg_color(g_src_dot, UI_COL_AMBER, 0);
        ui_set_label_text(g_lbl_status, syncing ? "正在请求天气数据..." : "等待网络同步");
        ui_set_label_text(g_lbl_time, "连接 Wi-Fi 或点击右上角「刷新」即可更新");
        return;
    }

    /* Outdoor temperature + comfort scale marker (-5°C..40°C -> x 2..406) */
    snprintf(text, sizeof(text), "%.1f°", info.temperature_tenths / 10.0);
    ui_set_label_text(g_lbl_temp, text);
    lv_obj_align_to(g_lbl_unit, g_lbl_temp, LV_ALIGN_OUT_RIGHT_TOP, 4, 8);

    int32_t clamped = info.temperature_tenths;
    if (clamped < -50) clamped = -50;
    if (clamped > 400) clamped = 400;
    int32_t dot_x = 2 + ((clamped + 50) * 404) / 450;
    lv_obj_set_pos(g_temp_dot, dot_x, 229);

    /* Outdoor Metrics */
    snprintf(text, sizeof(text), "%.1f ℃", info.apparent_tenths / 10.0);
    ui_set_label_text(g_lbl_app_val, info.humidity < 0 ? "--" : text);
    ui_set_label_text(g_lbl_app_tag, comfort_tag(info.apparent_tenths));

    if (info.humidity >= 0)
    {
        snprintf(text, sizeof(text), "%d%%", info.humidity);
        ui_set_label_text(g_lbl_humi_val, text);
        lv_bar_set_value(g_bar_humi, info.humidity, g_anim_running ? LV_ANIM_ON : LV_ANIM_OFF);
    }
    else
    {
        ui_set_label_text(g_lbl_humi_val, "--");
        lv_bar_set_value(g_bar_humi, 0, LV_ANIM_OFF);
    }

    if (info.wind_kmh_tenths >= 0)
    {
        snprintf(text, sizeof(text), "%.0f km/h", info.wind_kmh_tenths / 10.0);
        ui_set_label_text(g_lbl_wind_val, text);
    }
    else
    {
        ui_set_label_text(g_lbl_wind_val, "--");
    }
    ui_set_label_text(g_lbl_wind_tag, wind_level_tag(info.wind_kmh_tenths));

    /* Indoor vs Outdoor delta */
    if (aht_ok)
    {
        float out_c = info.temperature_tenths / 10.0f;
        float diff  = g_aht_temp - out_c;
        if (fabsf(diff) < 0.8f)
            ui_set_label_text(g_lbl_delta, "内外温差接近");
        else if (diff > 0.0f)
        {
            snprintf(text, sizeof(text), "车内高 +%.1f ℃", (double)diff);
            ui_set_label_text(g_lbl_delta, text);
        }
        else
        {
            snprintf(text, sizeof(text), "车内低 %.1f ℃", (double)diff);
            ui_set_label_text(g_lbl_delta, text);
        }

        if (g_aht_humi < 35.0f)
            ui_set_label_text(g_lbl_cabin_tip, "车内偏干 · 注意补水");
        else if (g_aht_humi > 70.0f)
            ui_set_label_text(g_lbl_cabin_tip, "湿度偏高 · 建议除湿通风");
        else if (diff > 8.0f)
            ui_set_label_text(g_lbl_cabin_tip, "温差较大 · 出门注意添衣");
        else
            ui_set_label_text(g_lbl_cabin_tip, "车内体感舒适宜人");
    }

    /* Footer source & observation time */
    bool fresh = ui_weather_is_fresh(&info, now);
    lv_obj_set_style_bg_color(g_src_dot, fresh ? UI_COL_GREEN : UI_COL_AMBER, 0);
    const char *src_str = info.source == WATCH_WEATHER_USB  ? "电脑串口" :
                          info.source == WATCH_WEATHER_WIFI ? "无线网络实况" : "局域网同步";
    snprintf(text, sizeof(text), "%s · %s", src_str, fresh ? "数据最新" : "等待刷新");
    ui_set_label_text(g_lbl_status, text);

    struct tm local;
    time_t at = info.weather_epoch + info.offset_minutes * 60;
    if (gmtime_r(&at, &local) != NULL)
    {
        snprintf(text, sizeof(text), "观测时间  %02d-%02d  %02d:%02d",
                 local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min);
        ui_set_label_text(g_lbl_time, text);
    }
}
