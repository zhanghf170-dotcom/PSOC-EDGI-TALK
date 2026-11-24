#ifndef UI_THEME_H__
#define UI_THEME_H__

#include <lvgl.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UI_SCREEN_W       480
#define UI_SCREEN_H       800
#define UI_TOPBAR_H       48
#define UI_NAVBAR_H       64
#define UI_BODY_H         (UI_SCREEN_H - UI_TOPBAR_H - UI_NAVBAR_H)

LV_FONT_DECLARE(ui_font_zh);
#define UI_FONT_ZH (&ui_font_zh)

/* Automotive Glassmorphism Dark Cockpit Palette */
#define UI_COL_BG         lv_color_hex(0x0B0F1A)
#define UI_COL_CARD       lv_color_hex(0x141C2E)
#define UI_COL_CARD_HI    lv_color_hex(0x1A253D)
#define UI_COL_BORDER     lv_color_hex(0x23304A)
#define UI_COL_TEXT       lv_color_hex(0xF1F5F9)
#define UI_COL_MUTED      lv_color_hex(0x94A3B8)
#define UI_COL_CYAN       lv_color_hex(0x38BDF8)
#define UI_COL_GREEN      lv_color_hex(0x10B981)
#define UI_COL_AMBER      lv_color_hex(0xF59E0B)
#define UI_COL_RED        lv_color_hex(0xEF4444)

typedef enum
{
    UI_SCR_HOME = 0,
    UI_SCR_FATIGUE,
    UI_SCR_WIFI,
    UI_SCR_SYSTEM,
    UI_SCR_APPS,
    UI_SCR_WEATHER,
    UI_SCR_BRIGHTNESS,
    UI_SCR_GALLERY,
    UI_SCR_CAMERA,
    UI_SCR_COUNT
} ui_screen_id_t;

typedef struct
{
    lv_obj_t *topbar;
    lv_obj_t *lbl_title;
    lv_obj_t *pill_core;
    lv_obj_t *lbl_core;
    lv_obj_t *lbl_wifi;
    lv_obj_t *pill_alert;
    lv_obj_t *lbl_alert;
    lv_obj_t *body;
    lv_obj_t *navbar;
} ui_chrome_t;

extern lv_style_t g_st_screen;
extern lv_style_t g_st_card;
extern lv_style_t g_st_pill;
extern lv_style_t g_st_btn_primary;
extern lv_style_t g_st_btn_danger;
extern lv_style_t g_st_btn_ghost;

void ui_theme_init(void);
void ui_create_chrome(lv_obj_t *scr, const char *title,
                      ui_screen_id_t active_tab, ui_chrome_t *refs);
void ui_update_chrome(ui_chrome_t *refs);
void ui_set_label_text(lv_obj_t *label, const char *text);

#ifdef __cplusplus
}
#endif

#endif /* UI_THEME_H__ */
