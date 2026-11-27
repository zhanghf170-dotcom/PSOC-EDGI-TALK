#ifndef UI_MANAGER_H__
#define UI_MANAGER_H__

#include "ui_theme.h"

#ifdef __cplusplus
extern "C" {
#endif

void ui_switch_to(ui_screen_id_t id);
void ui_switch_to_anim(ui_screen_id_t id, lv_screen_load_anim_t anim, uint32_t duration_ms);
ui_screen_id_t ui_get_active_screen(void);
void ui_set_pulldown_target(ui_screen_id_t id);
ui_screen_id_t ui_get_pulldown_target(void);
const char *ui_pulldown_target_name(ui_screen_id_t id);
void ui_handle_swipe(lv_dir_t dir);
void ui_request_toggle_fatigue(void);
void ui_request_open_xiaozhi_and_talk(void);
int  ui_request_page_by_name(const char *name);

lv_obj_t *ui_screen_home_create(void);
void      ui_screen_home_update(void);
lv_obj_t *ui_screen_apps_create(void);
void      ui_screen_apps_update(void);
lv_obj_t *ui_screen_weather_create(void);
void      ui_screen_weather_on_enter(void);
void      ui_screen_weather_on_leave(void);
void      ui_screen_weather_update(void);
lv_obj_t *ui_screen_brightness_create(void);
void      ui_screen_brightness_update(void);

lv_obj_t *ui_screen_gallery_create(void);
void      ui_screen_gallery_on_enter(void);
void      ui_screen_gallery_update(void);
void      ui_screen_gallery_on_leave(void);
void      ui_screen_gallery_open(unsigned index);
bool      ui_screen_gallery_handle_swipe(lv_dir_t dir);

lv_obj_t *ui_screen_music_create(void);
lv_obj_t *ui_screen_now_playing_create(void);
void      ui_screen_music_on_enter(void);
void      ui_screen_music_update(void);

lv_obj_t *ui_screen_camera_create(void);
void      ui_screen_camera_on_enter(void);
void      ui_screen_camera_on_leave(bool enter_monitor);
void      ui_screen_camera_update(void);

lv_obj_t *ui_screen_xiaozhi_create(void);
void      ui_screen_xiaozhi_on_enter(void);
void      ui_screen_xiaozhi_on_leave(void);
void      ui_screen_xiaozhi_update(void);

lv_obj_t *ui_screen_clock_tools_create(void);
void      ui_screen_clock_tools_open_tab(int tab);
bool      ui_screen_clock_tools_handle_swipe(lv_dir_t dir);
void      ui_screen_clock_tools_update(void);
void      ui_screen_clock_tools_bg_tick(void);
bool      ui_screen_clock_tools_is_running(void);

lv_obj_t *ui_brightness_slider_create(lv_obj_t *parent, lv_obj_t *value_label);

lv_obj_t *ui_screen_fatigue_create(void);
void      ui_screen_fatigue_open_hub(void);
void      ui_screen_fatigue_open_mode(int mode);
void      ui_screen_fatigue_on_enter(void);
void      ui_screen_fatigue_on_leave(void);
bool      ui_screen_fatigue_handle_swipe(lv_dir_t dir);
void      ui_screen_fatigue_update(void);

lv_obj_t *ui_screen_wifi_create(void);
void      ui_screen_wifi_update(void);

lv_obj_t *ui_screen_system_create(void);
void      ui_screen_system_update(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_MANAGER_H__ */
