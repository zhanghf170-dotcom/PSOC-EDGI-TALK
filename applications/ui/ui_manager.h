#ifndef UI_MANAGER_H__
#define UI_MANAGER_H__

#include "ui_theme.h"

#ifdef __cplusplus
extern "C" {
#endif

void ui_switch_to(ui_screen_id_t id);
ui_screen_id_t ui_get_active_screen(void);
void ui_request_toggle_fatigue(void);

lv_obj_t *ui_screen_home_create(void);
void      ui_screen_home_update(void);
lv_obj_t *ui_screen_apps_create(void);
void      ui_screen_apps_update(void);
lv_obj_t *ui_screen_weather_create(void);
void      ui_screen_weather_update(void);
lv_obj_t *ui_screen_brightness_create(void);
void      ui_screen_brightness_update(void);

lv_obj_t *ui_screen_gallery_create(void);
void      ui_screen_gallery_update(void);
void      ui_screen_gallery_on_leave(void);
void      ui_screen_gallery_open(unsigned index);

lv_obj_t *ui_screen_camera_create(void);
void      ui_screen_camera_on_enter(void);
void      ui_screen_camera_on_leave(bool enter_monitor);
void      ui_screen_camera_update(void);

lv_obj_t *ui_brightness_slider_create(lv_obj_t *parent, lv_obj_t *value_label);

lv_obj_t *ui_screen_fatigue_create(void);
void      ui_screen_fatigue_on_enter(void);
void      ui_screen_fatigue_update(void);

lv_obj_t *ui_screen_wifi_create(void);
void      ui_screen_wifi_update(void);

lv_obj_t *ui_screen_system_create(void);
void      ui_screen_system_update(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_MANAGER_H__ */
