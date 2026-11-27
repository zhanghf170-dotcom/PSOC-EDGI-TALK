#include <rtthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ui_manager.h"
#include "usbh_uvc_display_hook.h"
#include "uvc_ai_app.h"
#include "lv_port_disp.h"
#include "xiaozhi.h"

#define UI_GESTURE_CFG_PATH "/sdcard/ui_gesture.cfg"
#define UI_SWIPE_ANIM_MS    190U

extern uint32_t lcd_get_scanout_count(void);

static lv_obj_t *g_screens[UI_SCR_COUNT];
static volatile ui_screen_id_t g_active_id = UI_SCR_HOME;
static volatile ui_screen_id_t g_pulldown_target = UI_SCR_GALLERY; /* Default: Gallery */
static volatile bool g_entered_via_pulldown = false;
static volatile rt_bool_t g_toggle_fatigue_req = RT_FALSE;
static volatile rt_bool_t g_xiaozhi_talk_req = RT_FALSE;
static volatile rt_bool_t g_fps_test_req = RT_FALSE;
static volatile lv_dir_t  g_requested_swipe = LV_DIR_NONE;
static uint32_t g_last_update;
static volatile int g_requested_page = -1;
static volatile int g_requested_clock_tab = -1;
static volatile int g_requested_ai_mode = -1;
static volatile unsigned g_requested_gallery_item;

/* Primary carousel order for left/right horizontal swipe */
static const ui_screen_id_t g_carousel[] = {
    UI_SCR_HOME,     /* EDGI 表盘 */
    UI_SCR_WEATHER,  /* 天气实况 */
    UI_SCR_XIAOZHI,  /* Gemini娘 · 小智AI */
    UI_SCR_FATIGUE,  /* 疲劳监测 */
    UI_SCR_APPS,     /* 应用列表 */
};
#define UI_CAROUSEL_LEN (sizeof(g_carousel) / sizeof(g_carousel[0]))

static bool is_valid_pulldown_target(ui_screen_id_t id)
{
    switch (id)
    {
    case UI_SCR_GALLERY:
    case UI_SCR_MUSIC:
    case UI_SCR_WEATHER:
    case UI_SCR_XIAOZHI:
    case UI_SCR_FATIGUE:
    case UI_SCR_BRIGHTNESS:
    case UI_SCR_WIFI:
    case UI_SCR_SYSTEM:
    case UI_SCR_CLOCK_TOOLS:
        return true;
    default:
        return false;
    }
}

const char *ui_pulldown_target_name(ui_screen_id_t id)
{
    switch (id)
    {
    case UI_SCR_GALLERY:    return "图库";
    case UI_SCR_MUSIC:      return "音乐";
    case UI_SCR_WEATHER:    return "天气";
    case UI_SCR_XIAOZHI:    return "小智";
    case UI_SCR_FATIGUE:    return "监测";
    case UI_SCR_BRIGHTNESS: return "亮度";
    case UI_SCR_WIFI:       return "无线网络";
    case UI_SCR_SYSTEM:     return "设置";
    case UI_SCR_CLOCK_TOOLS:return "日历计时";
    default:                return "图库";
    }
}

static const char *pulldown_id_to_key(ui_screen_id_t id)
{
    switch (id)
    {
    case UI_SCR_GALLERY:    return "gallery";
    case UI_SCR_MUSIC:      return "music";
    case UI_SCR_WEATHER:    return "weather";
    case UI_SCR_XIAOZHI:    return "xiaozhi";
    case UI_SCR_FATIGUE:    return "fatigue";
    case UI_SCR_BRIGHTNESS: return "brightness";
    case UI_SCR_WIFI:       return "wifi";
    case UI_SCR_SYSTEM:     return "system";
    case UI_SCR_CLOCK_TOOLS:return "clock";
    default:                return "gallery";
    }
}

static bool pulldown_key_to_id(const char *key, ui_screen_id_t *out)
{
    if (key == NULL || out == NULL) return false;
    if (strcmp(key, "gallery") == 0)    { *out = UI_SCR_GALLERY;    return true; }
    if (strcmp(key, "music") == 0)      { *out = UI_SCR_MUSIC;      return true; }
    if (strcmp(key, "weather") == 0)    { *out = UI_SCR_WEATHER;    return true; }
    if (strcmp(key, "xiaozhi") == 0 || strcmp(key, "gemini") == 0)
                                        { *out = UI_SCR_XIAOZHI;    return true; }
    if (strcmp(key, "fatigue") == 0 || strcmp(key, "monitor") == 0)
                                        { *out = UI_SCR_FATIGUE;    return true; }
    if (strcmp(key, "brightness") == 0) { *out = UI_SCR_BRIGHTNESS; return true; }
    if (strcmp(key, "wifi") == 0)       { *out = UI_SCR_WIFI;        return true; }
    if (strcmp(key, "system") == 0 || strcmp(key, "settings") == 0 || strcmp(key, "volume") == 0)
                                        { *out = UI_SCR_SYSTEM;      return true; }
    if (strcmp(key, "clock") == 0 || strcmp(key, "calendar") == 0)
                                        { *out = UI_SCR_CLOCK_TOOLS; return true; }
    return false;
}

static void ui_load_gesture_cfg(void)
{
    FILE *fp = fopen(UI_GESTURE_CFG_PATH, "r");
    if (fp == NULL) return;
    char line[64];
    while (fgets(line, sizeof(line), fp) != NULL)
    {
        size_t n = strlen(line);
        while (n > 0U && (line[n - 1U] == '\r' || line[n - 1U] == '\n' || line[n - 1U] == ' '))
            line[--n] = '\0';
        if (strncmp(line, "pulldown=", 9) == 0)
        {
            ui_screen_id_t parsed = UI_SCR_GALLERY;
            if (pulldown_key_to_id(line + 9, &parsed))
                g_pulldown_target = parsed;
        }
    }
    fclose(fp);
}

static void ui_save_gesture_cfg(void)
{
    FILE *fp = fopen(UI_GESTURE_CFG_PATH, "w");
    if (fp == NULL) return;
    fprintf(fp, "pulldown=%s\n", pulldown_id_to_key(g_pulldown_target));
    fclose(fp);
}

void ui_set_pulldown_target(ui_screen_id_t id)
{
    if (!is_valid_pulldown_target(id)) return;
    g_pulldown_target = id;
    ui_save_gesture_cfg();
}

ui_screen_id_t ui_get_pulldown_target(void)
{
    return g_pulldown_target;
}

void ui_switch_to_anim(ui_screen_id_t id, lv_screen_load_anim_t anim, uint32_t duration_ms)
{
    if ((id >= UI_SCR_COUNT) || (g_screens[id] == NULL))
    {
        return;
    }

    if (id != g_active_id)
    {
        if (g_active_id == UI_SCR_CAMERA)
            ui_screen_camera_on_leave(id == UI_SCR_FATIGUE);
        else if (g_active_id == UI_SCR_FATIGUE && id != UI_SCR_CAMERA)
            ui_screen_fatigue_on_leave();
        else if (g_active_id == UI_SCR_GALLERY)
            ui_screen_gallery_on_leave();
        else if (g_active_id == UI_SCR_WEATHER)
            ui_screen_weather_on_leave();
        else if (g_active_id == UI_SCR_XIAOZHI)
            ui_screen_xiaozhi_on_leave();

        if (id == UI_SCR_HOME)
            g_entered_via_pulldown = false;

        g_active_id = id;
        g_last_update = lv_tick_get() - 500U;
        lv_screen_load_anim(g_screens[id], anim,
                            anim == LV_SCR_LOAD_ANIM_NONE ? 0U : duration_ms,
                            0, false);
        if (id == UI_SCR_FATIGUE)
        {
            ui_screen_fatigue_on_enter();
        }
        else if (id == UI_SCR_CAMERA)
        {
            ui_screen_camera_on_enter();
        }
        else if (id == UI_SCR_XIAOZHI)
        {
            ui_screen_xiaozhi_on_enter();
        }
        else if (id == UI_SCR_WEATHER)
        {
            ui_screen_weather_on_enter();
        }
        else if (id == UI_SCR_GALLERY)
        {
            ui_screen_gallery_on_enter();
        }
        else if (id == UI_SCR_MUSIC || id == UI_SCR_NOW_PLAYING)
        {
            ui_screen_music_on_enter();
        }
    }
}

void ui_switch_to(ui_screen_id_t id)
{
    lv_screen_load_anim_t animation = LV_SCR_LOAD_ANIM_NONE;
    if (id == UI_SCR_APPS && g_active_id == UI_SCR_HOME)
        animation = LV_SCR_LOAD_ANIM_OVER_TOP;
    else if (id == UI_SCR_HOME && g_active_id == UI_SCR_APPS)
        animation = LV_SCR_LOAD_ANIM_OUT_BOTTOM;

    g_entered_via_pulldown = false;
    ui_switch_to_anim(id, animation, 180U);
}

static int find_in_carousel(ui_screen_id_t id)
{
    for (size_t i = 0; i < UI_CAROUSEL_LEN; i++)
    {
        if (g_carousel[i] == id) return (int)i;
    }
    return -1;
}

void ui_handle_swipe(lv_dir_t dir)
{
    if (dir == LV_DIR_NONE) return;

    /* Give Gallery full-screen viewer first chance at swipe (prev/next photo or back to grid) */
    if (g_active_id == UI_SCR_GALLERY && ui_screen_gallery_handle_swipe(dir))
    {
        return;
    }
    if (g_active_id == UI_SCR_CLOCK_TOOLS && ui_screen_clock_tools_handle_swipe(dir))
    {
        return;
    }
    if (g_active_id == UI_SCR_FATIGUE && ui_screen_fatigue_handle_swipe(dir))
    {
        return;
    }
    if (g_active_id == UI_SCR_NOW_PLAYING && (dir == LV_DIR_RIGHT || dir == LV_DIR_BOTTOM))
    {
        ui_switch_to(UI_SCR_MUSIC);
        return;
    }

    /* Spatial Watch Face gestures:
     *   Above = Gallery/Shortcut (swipe DOWN to pull down)
     *   Below = Apps (swipe UP to bring up)
     *   Left  = AI Hub (swipe RIGHT to move to left page)
     *   Right = Gemini-chan (swipe LEFT to move to right page)
     */
    if (g_active_id == UI_SCR_HOME)
    {
        if (dir == LV_DIR_BOTTOM)
        {
            ui_screen_id_t target = g_pulldown_target;
            if (!is_valid_pulldown_target(target)) target = UI_SCR_GALLERY;
            g_entered_via_pulldown = true;
            ui_switch_to_anim(target, LV_SCR_LOAD_ANIM_OVER_BOTTOM, UI_SWIPE_ANIM_MS);
            return;
        }
        if (dir == LV_DIR_TOP)
        {
            g_entered_via_pulldown = false;
            ui_switch_to_anim(UI_SCR_APPS, LV_SCR_LOAD_ANIM_OVER_TOP, UI_SWIPE_ANIM_MS);
            return;
        }
        if (dir == LV_DIR_RIGHT)
        {
            g_entered_via_pulldown = false;
            ui_screen_fatigue_open_hub();
            ui_switch_to_anim(UI_SCR_FATIGUE, LV_SCR_LOAD_ANIM_MOVE_RIGHT, UI_SWIPE_ANIM_MS);
            return;
        }
        if (dir == LV_DIR_LEFT)
        {
            g_entered_via_pulldown = false;
            ui_switch_to_anim(UI_SCR_XIAOZHI, LV_SCR_LOAD_ANIM_MOVE_LEFT, UI_SWIPE_ANIM_MS);
            return;
        }
    }

    /* Apps page (below Home): swipe down/left/right returns to Watch Face */
    if (g_active_id == UI_SCR_APPS)
    {
        if (dir == LV_DIR_BOTTOM)
        {
            ui_switch_to_anim(UI_SCR_HOME, LV_SCR_LOAD_ANIM_OUT_BOTTOM, UI_SWIPE_ANIM_MS);
            return;
        }
        if (dir == LV_DIR_RIGHT || dir == LV_DIR_LEFT)
        {
            ui_switch_to_anim(UI_SCR_HOME,
                              dir == LV_DIR_LEFT ? LV_SCR_LOAD_ANIM_MOVE_LEFT : LV_SCR_LOAD_ANIM_MOVE_RIGHT,
                              UI_SWIPE_ANIM_MS);
            return;
        }
    }

    /* Symmetric return gestures from Gemini-chan (right) and AI Hub (left) */
    if (g_active_id == UI_SCR_XIAOZHI && dir == LV_DIR_RIGHT)
    {
        ui_switch_to_anim(UI_SCR_HOME, LV_SCR_LOAD_ANIM_MOVE_RIGHT, UI_SWIPE_ANIM_MS);
        return;
    }
    if (g_active_id == UI_SCR_FATIGUE && dir == LV_DIR_LEFT)
    {
        ui_switch_to_anim(UI_SCR_HOME, LV_SCR_LOAD_ANIM_MOVE_LEFT, UI_SWIPE_ANIM_MS);
        return;
    }

    /* Any page opened via Watch Face pull-down (or Gallery grid): swipe up returns to Watch Face */
    if (dir == LV_DIR_TOP && (g_entered_via_pulldown || g_active_id == UI_SCR_GALLERY))
    {
        g_entered_via_pulldown = false;
        ui_switch_to_anim(UI_SCR_HOME, LV_SCR_LOAD_ANIM_OUT_TOP, UI_SWIPE_ANIM_MS);
        return;
    }

    /* Global Left / Right horizontal carousel swipe */
    if (dir == LV_DIR_LEFT || dir == LV_DIR_RIGHT)
    {
        int idx = find_in_carousel(g_active_id);
        if (idx >= 0)
        {
            int next_idx = (dir == LV_DIR_LEFT)
                               ? (idx + 1) % (int)UI_CAROUSEL_LEN
                               : (idx + (int)UI_CAROUSEL_LEN - 1) % (int)UI_CAROUSEL_LEN;
            g_entered_via_pulldown = false;
            ui_switch_to_anim(g_carousel[next_idx],
                              dir == LV_DIR_LEFT ? LV_SCR_LOAD_ANIM_MOVE_LEFT : LV_SCR_LOAD_ANIM_MOVE_RIGHT,
                              UI_SWIPE_ANIM_MS);
        }
        else
        {
            /* Sub-pages (Gallery grid / System / Wi-Fi / Brightness / Camera): swipe right -> back, swipe left -> Home */
            ui_screen_id_t dest = (dir == LV_DIR_RIGHT)
                                      ? (g_entered_via_pulldown ? UI_SCR_HOME : UI_SCR_APPS)
                                      : UI_SCR_HOME;
            g_entered_via_pulldown = false;
            ui_switch_to_anim(dest,
                              dir == LV_DIR_LEFT ? LV_SCR_LOAD_ANIM_MOVE_LEFT : LV_SCR_LOAD_ANIM_MOVE_RIGHT,
                              UI_SWIPE_ANIM_MS);
        }
    }
}

static void screen_gesture_event_cb(lv_event_t *e)
{
    (void)e;
    lv_indev_t *indev = lv_indev_active();
    if (indev == NULL) return;

    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    if (dir == LV_DIR_NONE) return;

    lv_indev_wait_release(indev);
    ui_handle_swipe(dir);
}

ui_screen_id_t ui_get_active_screen(void)
{
    return g_active_id;
}

void ui_request_toggle_fatigue(void)
{
    if (g_active_id != UI_SCR_CAMERA)
        g_toggle_fatigue_req = RT_TRUE;
}

void ui_request_open_xiaozhi_and_talk(void)
{
    g_xiaozhi_talk_req = RT_TRUE;
}

int ui_request_page_by_name(const char *name)
{
    static const struct { const char *name; ui_screen_id_t id; } map[] = {
        {"face",       UI_SCR_HOME},
        {"home",       UI_SCR_HOME},
        {"ai",         UI_SCR_FATIGUE},
        {"monitor",    UI_SCR_FATIGUE},
        {"fatigue",    UI_SCR_FATIGUE},
        {"steel_ball", UI_SCR_FATIGUE},
        {"ball",       UI_SCR_FATIGUE},
        {"drowsy",     UI_SCR_FATIGUE},
        {"fatigue_det",UI_SCR_FATIGUE},
        {"wifi",       UI_SCR_WIFI},
        {"system",     UI_SCR_SYSTEM},
        {"settings",   UI_SCR_SYSTEM},
        {"volume",     UI_SCR_SYSTEM},
        {"apps",       UI_SCR_APPS},
        {"music",      UI_SCR_MUSIC},
        {"song",       UI_SCR_NOW_PLAYING},
        {"weather",    UI_SCR_WEATHER},
        {"brightness", UI_SCR_BRIGHTNESS},
        {"gallery",    UI_SCR_GALLERY},
        {"camera",     UI_SCR_CAMERA},
        {"xiaozhi",    UI_SCR_XIAOZHI},
        {"gemini",     UI_SCR_XIAOZHI},
        {"clock",      UI_SCR_CLOCK_TOOLS},
        {"calendar",   UI_SCR_CLOCK_TOOLS},
        {"stopwatch",  UI_SCR_CLOCK_TOOLS},
        {"countdown",  UI_SCR_CLOCK_TOOLS},
        {"timer",      UI_SCR_CLOCK_TOOLS},
    };
    if (name == NULL) return -RT_EINVAL;
    for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); i++)
    {
        if (strcmp(name, map[i].name) == 0)
        {
            int ctab = -1;
            int aimode = -1;
            if (strcmp(name, "calendar") == 0) ctab = 0;
            else if (strcmp(name, "stopwatch") == 0) ctab = 1;
            else if (strcmp(name, "countdown") == 0 || strcmp(name, "timer") == 0) ctab = 2;
            else if (strcmp(name, "steel_ball") == 0 || strcmp(name, "ball") == 0) aimode = UVC_AI_MODE_STEEL_BALL;
            else if (strcmp(name, "drowsy") == 0 || strcmp(name, "fatigue_det") == 0) aimode = UVC_AI_MODE_FATIGUE;

            rt_base_t level = rt_hw_interrupt_disable();
            g_requested_gallery_item = 0;
            g_requested_clock_tab = ctab;
            g_requested_ai_mode = aimode;
            g_requested_page = (int)map[i].id;
            rt_hw_interrupt_enable(level);
            return RT_EOK;
        }
    }
    return -RT_EINVAL;
}

static void move_fps_marker(void *object, int32_t x)
{
    lv_obj_set_x(object, x);
}

static void start_fps_animation(void)
{
    lv_anim_t animation;
    lv_obj_t *marker = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(marker);
    lv_obj_remove_flag(marker, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(marker, 26, 6);
    lv_obj_set_pos(marker, 8, UI_TOPBAR_H + 4);
    lv_obj_set_style_radius(marker, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(marker, UI_COL_CYAN, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(marker, LV_OPA_COVER, LV_PART_MAIN);

    lv_anim_init(&animation);
    lv_anim_set_var(&animation, marker);
    lv_anim_set_values(&animation, 8, UI_SCREEN_W - 34);
    lv_anim_set_duration(&animation, 4000);
    lv_anim_set_exec_cb(&animation, move_fps_marker);
    lv_anim_start(&animation);
    lv_obj_delete_delayed(marker, 4000);
}

static void ui_periodic_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    rt_base_t level = rt_hw_interrupt_disable();
    int requested = g_requested_page;
    int req_ctab  = g_requested_clock_tab;
    int req_aimode = g_requested_ai_mode;
    unsigned item = g_requested_gallery_item;
    lv_dir_t req_swipe = g_requested_swipe;
    g_requested_page = -1;
    g_requested_clock_tab = -1;
    g_requested_ai_mode = -1;
    g_requested_swipe = LV_DIR_NONE;
    rt_hw_interrupt_enable(level);

    if (requested >= 0)
    {
        if (requested == UI_SCR_CLOCK_TOOLS && req_ctab >= 0)
            ui_screen_clock_tools_open_tab(req_ctab);
        else if (requested == UI_SCR_FATIGUE)
        {
            if (req_aimode >= 0) ui_screen_fatigue_open_mode(req_aimode);
            else ui_screen_fatigue_open_hub();
        }
        ui_switch_to((ui_screen_id_t)requested);
        if (requested == UI_SCR_GALLERY && item != 0)
            ui_screen_gallery_open(item - 1);
    }

    ui_screen_clock_tools_bg_tick();

    if (req_swipe != LV_DIR_NONE)
    {
        ui_handle_swipe(req_swipe);
    }

    if (g_fps_test_req)
    {
        g_fps_test_req = RT_FALSE;
        start_fps_animation();
    }

    if (g_xiaozhi_talk_req)
    {
        g_xiaozhi_talk_req = RT_FALSE;
        if (g_active_id != UI_SCR_XIAOZHI)
        {
            ui_switch_to(UI_SCR_XIAOZHI);
        }
        xiaozhi_trigger_talk();
    }

    if (g_toggle_fatigue_req)
    {
        g_toggle_fatigue_req = RT_FALSE;
        if (g_active_id == UI_SCR_FATIGUE)
        {
            (void)uvc_app_stop();
            ui_switch_to(UI_SCR_HOME);
        }
        else if (g_active_id != UI_SCR_CAMERA)
        {
            ui_screen_fatigue_open_hub();
            ui_switch_to(UI_SCR_FATIGUE);
        }
    }

    bool camera_page = g_active_id == UI_SCR_FATIGUE || g_active_id == UI_SCR_CAMERA;
    bool fast_clock  = (g_active_id == UI_SCR_CLOCK_TOOLS && ui_screen_clock_tools_is_running());
    uint32_t period = camera_page ? 32U
                    : (g_active_id == UI_SCR_NOW_PLAYING ? 80U
                    : (fast_clock ? 40U
                    : (g_active_id == UI_SCR_XIAOZHI ? 60U : 250U)));
    if (lv_tick_elaps(g_last_update) < period)
        return;
    g_last_update = lv_tick_get();

    switch (g_active_id)
    {
    case UI_SCR_HOME:
        ui_screen_home_update();
        break;
    case UI_SCR_FATIGUE:
        ui_screen_fatigue_update();
        break;
    case UI_SCR_WIFI:
        ui_screen_wifi_update();
        break;
    case UI_SCR_SYSTEM:
        ui_screen_system_update();
        break;
    case UI_SCR_APPS:
        ui_screen_apps_update();
        break;
    case UI_SCR_WEATHER:
        ui_screen_weather_update();
        break;
    case UI_SCR_BRIGHTNESS:
        ui_screen_brightness_update();
        break;
    case UI_SCR_GALLERY:
        ui_screen_gallery_update();
        break;
    case UI_SCR_CAMERA:
        ui_screen_camera_update();
        break;
    case UI_SCR_XIAOZHI:
        ui_screen_xiaozhi_update();
        break;
    case UI_SCR_CLOCK_TOOLS:
        ui_screen_clock_tools_update();
        break;
    case UI_SCR_MUSIC:
    case UI_SCR_NOW_PLAYING:
        ui_screen_music_update();
        break;
    default:
        break;
    }
}

void lv_user_gui_init(void)
{
    ui_theme_init();
    ui_load_gesture_cfg();

    g_screens[UI_SCR_HOME]       = ui_screen_home_create();
    g_screens[UI_SCR_FATIGUE]    = ui_screen_fatigue_create();
    g_screens[UI_SCR_WIFI]       = ui_screen_wifi_create();
    g_screens[UI_SCR_SYSTEM]     = ui_screen_system_create();
    g_screens[UI_SCR_APPS]       = ui_screen_apps_create();
    g_screens[UI_SCR_WEATHER]    = ui_screen_weather_create();
    g_screens[UI_SCR_BRIGHTNESS] = ui_screen_brightness_create();
    g_screens[UI_SCR_GALLERY]     = ui_screen_gallery_create();
    g_screens[UI_SCR_CAMERA]      = ui_screen_camera_create();
    g_screens[UI_SCR_XIAOZHI]     = ui_screen_xiaozhi_create();
    g_screens[UI_SCR_CLOCK_TOOLS] = ui_screen_clock_tools_create();
    g_screens[UI_SCR_MUSIC]       = ui_screen_music_create();
    g_screens[UI_SCR_NOW_PLAYING] = ui_screen_now_playing_create();

    /* Attach unified left/right/up/down swipe gesture handler to every screen */
    for (unsigned i = 0; i < UI_SCR_COUNT; i++)
    {
        if (g_screens[i] != NULL)
        {
            lv_obj_add_event_cb(g_screens[i], screen_gesture_event_cb, LV_EVENT_GESTURE, NULL);
        }
    }

    g_active_id = UI_SCR_HOME;
    lv_screen_load(g_screens[UI_SCR_HOME]);

    lv_timer_create(ui_periodic_timer_cb, 16, NULL);
}

static int ui_fps(void)
{
    if (g_screens[UI_SCR_HOME] == NULL)
        return -RT_ERROR;

    g_fps_test_req = RT_TRUE;
    rt_thread_mdelay(250);
    rt_tick_t begin = rt_tick_get();
    uint32_t scanouts = lcd_get_scanout_count();
    uint32_t presents = lv_port_disp_get_present_count();
    rt_thread_mdelay(2500);
    uint32_t elapsed = rt_tick_get() - begin;
    uint32_t scan_fps10 = (lcd_get_scanout_count() - scanouts) * (10U * RT_TICK_PER_SECOND) / elapsed;
    uint32_t ui_fps10 = (lv_port_disp_get_present_count() - presents) * (10U * RT_TICK_PER_SECOND) / elapsed;
    rt_kprintf("LCD scanout %u.%u Hz; LVGL partial animation %u.%u FPS\n",
               scan_fps10 / 10U, scan_fps10 % 10U, ui_fps10 / 10U, ui_fps10 % 10U);
    return RT_EOK;
}
MSH_CMD_EXPORT(ui_fps, Measure LCD scanout and LVGL partial animation rate);

static int ui_page(int argc, char **argv)
{
    unsigned item = 0;
    if (argc != 2 && argc != 3)
        return -RT_EINVAL;
    if (argc == 3)
    {
        if (strcmp(argv[1], "gallery") != 0 || strlen(argv[2]) != 1 ||
            argv[2][0] < '1' || argv[2][0] > '4')
            return -RT_EINVAL;
        item = (unsigned)(argv[2][0] - '0');
        rt_base_t level = rt_hw_interrupt_disable();
        g_requested_gallery_item = item;
        g_requested_page = (int)UI_SCR_GALLERY;
        rt_hw_interrupt_enable(level);
        return RT_EOK;
    }
    return ui_request_page_by_name(argv[1]);
}
MSH_CMD_EXPORT(ui_page, Open a UI page by name);

static int ui_pulldown(int argc, char **argv)
{
    if (argc < 2)
    {
        rt_kprintf("Watch pull-down target: %s (%s)\n",
                   pulldown_id_to_key(g_pulldown_target),
                   ui_pulldown_target_name(g_pulldown_target));
        rt_kprintf("Usage: ui_pulldown <gallery|weather|xiaozhi|fatigue|brightness|wifi|system>\n");
        return RT_EOK;
    }
    ui_screen_id_t target = UI_SCR_GALLERY;
    if (!pulldown_key_to_id(argv[1], &target))
    {
        rt_kprintf("Unknown target '%s'\n", argv[1]);
        return -RT_EINVAL;
    }
    ui_set_pulldown_target(target);
    rt_kprintf("Pull-down target set to: %s (%s)\n",
               pulldown_id_to_key(target), ui_pulldown_target_name(target));
    return RT_EOK;
}
MSH_CMD_EXPORT(ui_pulldown, Get or set Watch Face pull-down shortcut page);

static int ui_swipe(int argc, char **argv)
{
    if (argc < 2)
    {
        rt_kprintf("Usage: ui_swipe <left|right|up|down>\n");
        return -RT_EINVAL;
    }
    lv_dir_t dir = LV_DIR_NONE;
    if (strcmp(argv[1], "left") == 0)       dir = LV_DIR_LEFT;
    else if (strcmp(argv[1], "right") == 0) dir = LV_DIR_RIGHT;
    else if (strcmp(argv[1], "up") == 0)    dir = LV_DIR_TOP;
    else if (strcmp(argv[1], "down") == 0)  dir = LV_DIR_BOTTOM;
    else return -RT_EINVAL;

    rt_base_t level = rt_hw_interrupt_disable();
    g_requested_swipe = dir;
    rt_hw_interrupt_enable(level);
    return RT_EOK;
}
MSH_CMD_EXPORT(ui_swipe, Simulate touch swipe gesture: left|right|up|down);
