#ifndef SD_ASSETS_H__
#define SD_ASSETS_H__

#include <rtthread.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* TF card (/sdcard) asset paths */
#define SD_ALARM_WAV_PATH           "/sdcard/audio/cockpit_alarm.wav"
#define SD_DING_WAV_PATH            "/sdcard/audio/xiaozhi_ding.wav"
#define SD_AUDIO_CFG_PATH           "/sdcard/audio.cfg"

#define SD_GALLERY_THUMBS_PATH      "/sdcard/gallery/thumbnails.rgb565"
#define SD_GALLERY_PHOTOS_PATH      "/sdcard/gallery/photos.rgb565"
#define SD_GALLERY_GIF_PATH         "/sdcard/gallery/blink.gif"
#define SD_GALLERY_GIF_LV_PATH      "S:/sdcard/gallery/blink.gif"

#define SD_WALLPAPER_PATH           "/sdcard/res/watch_wallpaper.rgb565"
#define SD_GEMINI_DIR               "/sdcard/res/gemini"
#define SD_GEMINI_AVATAR_PATH       "/sdcard/res/gemini/gemini_avatar_64.rgb565"
#define SD_GEMINI_VOICES_PATH       "/sdcard/res/gemini/gemini_voices.bin"

#define SD_STEEL_BALL_MODEL_PATH    "/sdcard/models/steel_ball_model.bin"
#define SD_MUSIC_DIR                "/sdcard/music"
#define SD_CALENDAR_PATH            "/sdcard/calendar.txt"

/* Implemented in libraries/Common/board/ports/filesystem/mnt.c */
rt_bool_t sdcard_is_mounted(void);
uint32_t  sdcard_get_mount_gen(void);
rt_bool_t sdcard_wait_mounted(rt_int32_t timeout_ms);
rt_bool_t sdcard_is_present(void);
void      sdcard_request_remount(void);

/* Implemented in libraries/HAL_Drivers/drv_sdio.c */
rt_bool_t   sdio1_is_card_present(void);
rt_uint32_t sdio1_get_err_streak(void);
rt_uint32_t sdio1_get_total_errors(void);
rt_uint32_t sdio1_get_clock(void);
void        sdio1_trigger_redetect(rt_bool_t probe_after_remove);

/* Read [offset, offset + len) from a file into dst, cleaning D-Cache if requested */
int  sd_asset_read_range(const char *path, size_t offset, void *dst, size_t len, rt_bool_t clean_dcache);
int  sd_asset_read_file(const char *path, void *dst, size_t max_len, size_t *out_len, rt_bool_t clean_dcache);
void sd_asset_dcache_clean(void *ptr, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* SD_ASSETS_H__ */
