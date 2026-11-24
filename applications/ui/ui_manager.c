#include <rtthread.h>
#include <string.h>
#include "ui_manager.h"
#include "usbh_uvc_display_hook.h"
#include "uvc_ai_app.h"
#include "lv_port_disp.h"

extern uint32_t lcd_get_scanout_count(void);

static lv_obj_t *g_screens[UI_SCR_COUNT];
static volatile ui_screen_id_t g_active_id = UI_SCR_HOME;
static volatile rt_bool_t g_toggle_fatigue_req = RT_FALSE;
static volatile rt_bool_t g_fps_test_req = RT_FALSE;
static uint32_t g_last_update;
static volatile int g_requested_page = -1;
static volatile unsigned g_requested_gallery_item;

void ui_switch_to(ui_screen_id_t id)
{
    if ((id >= UI_SCR_COUNT) || (g_screens[id] == NULL))
    {
        return;
    }

    if (id != g_active_id)
    {
        lv_screen_load_anim_t animation = LV_SCR_LOAD_ANIM_NONE;
        if (id == UI_SCR_APPS && g_active_id == UI_SCR_HOME)
            animation = LV_SCR_LOAD_ANIM_OVER_TOP;
        else if (id == UI_SCR_HOME && g_active_id == UI_SCR_APPS)
            animation = LV_SCR_LOAD_ANIM_OUT_BOTTOM;

        if (g_active_id == UI_SCR_CAMERA)
            ui_screen_camera_on_leave(id == UI_SCR_FATIGUE);
        else if (g_active_id == UI_SCR_GALLERY)
            ui_screen_gallery_on_leave();

        g_active_id = id;
        g_last_update = lv_tick_get() - 500U;
        lv_screen_load_anim(g_screens[id], animation, animation == LV_SCR_LOAD_ANIM_NONE ? 0U : 180U,
                            0, false);
        if (id == UI_SCR_FATIGUE)
        {
            ui_screen_fatigue_on_enter();
        }
        else if (id == UI_SCR_CAMERA)
        {
            ui_screen_camera_on_enter();
        }
    }
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
    unsigned item = g_requested_gallery_item;
    g_requested_page = -1;
    rt_hw_interrupt_enable(level);
    if (requested >= 0)
    {
        ui_switch_to((ui_screen_id_t)requested);
        if (requested == UI_SCR_GALLERY && item != 0)
            ui_screen_gallery_open(item - 1);
    }

    if (g_fps_test_req)
    {
        g_fps_test_req = RT_FALSE;
        start_fps_animation();
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
            ui_switch_to(UI_SCR_FATIGUE);
        }
    }

    bool camera_page = g_active_id == UI_SCR_FATIGUE || g_active_id == UI_SCR_CAMERA;
    if (lv_tick_elaps(g_last_update) < (camera_page ? 32U : 250U))
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
    default:
        break;
    }
}

void lv_user_gui_init(void)
{
    ui_theme_init();

    g_screens[UI_SCR_HOME]    = ui_screen_home_create();
    g_screens[UI_SCR_FATIGUE] = ui_screen_fatigue_create();
    g_screens[UI_SCR_WIFI]    = ui_screen_wifi_create();
    g_screens[UI_SCR_SYSTEM]  = ui_screen_system_create();
    g_screens[UI_SCR_APPS]    = ui_screen_apps_create();
    g_screens[UI_SCR_WEATHER] = ui_screen_weather_create();
    g_screens[UI_SCR_BRIGHTNESS] = ui_screen_brightness_create();
    g_screens[UI_SCR_GALLERY] = ui_screen_gallery_create();
    g_screens[UI_SCR_CAMERA] = ui_screen_camera_create();

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
    static const char *const pages[] = {
        "face", "monitor", "wifi", "system", "apps", "weather", "brightness", "gallery", "camera"
    };
    unsigned item = 0;
    if (argc != 2 && argc != 3)
        return -RT_EINVAL;
    if (argc == 3)
    {
        if (strcmp(argv[1], "gallery") != 0 || strlen(argv[2]) != 1 ||
            argv[2][0] < '1' || argv[2][0] > '4')
            return -RT_EINVAL;
        item = (unsigned)(argv[2][0] - '0');
    }
    for (unsigned i = 0; i < sizeof(pages) / sizeof(pages[0]); i++)
    {
        if (strcmp(argv[1], pages[i]) == 0)
        {
            rt_base_t level = rt_hw_interrupt_disable();
            g_requested_gallery_item = item;
            g_requested_page = (int)i;
            rt_hw_interrupt_enable(level);
            return RT_EOK;
        }
    }
    return -RT_EINVAL;
}
MSH_CMD_EXPORT(ui_page, Open a UI page by name);
