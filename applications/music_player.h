#ifndef MUSIC_PLAYER_H__
#define MUSIC_PLAYER_H__

#include <rtthread.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MUSIC_MAX_TRACKS     64U
#define MUSIC_PATH_MAX       192U
#define MUSIC_TITLE_MAX      96U

typedef enum
{
    MUSIC_MODE_SEQ = 0,       /* Sequential: play through list once and stop */
    MUSIC_MODE_LOOP_ALL,      /* Repeat All: loop back to first track */
    MUSIC_MODE_LOOP_ONE,      /* Single Loop: repeat current track */
    MUSIC_MODE_SHUFFLE,       /* Random / Shuffle playback */
    MUSIC_MODE_COUNT
} music_play_mode_t;

typedef enum
{
    MUSIC_STATE_STOPPED = 0,
    MUSIC_STATE_PLAYING,
    MUSIC_STATE_PAUSED
} music_state_t;

typedef enum
{
    MUSIC_FMT_NONE = 0,
    MUSIC_FMT_WAV,
    MUSIC_FMT_MP3
} music_format_t;

typedef struct
{
    char           path[MUSIC_PATH_MAX];
    char           title[MUSIC_TITLE_MAX];
    music_format_t format;
    uint32_t       file_size;
} music_track_info_t;

typedef struct
{
    music_state_t     state;
    music_play_mode_t mode;
    uint32_t          track_count;
    int               current_index;
    char              title[MUSIC_TITLE_MAX];
    music_format_t    format;
    uint32_t          sample_rate;
    uint16_t          channels;
    uint32_t          elapsed_sec;
    uint32_t          elapsed_ms;
    uint32_t          duration_sec;
    uint16_t          progress_permille;  /* 0..1000 */
    rt_bool_t         sd_mounted;
    uint32_t          library_rev;
    uint32_t          rev;
} music_status_t;

int  music_player_init(void);
int  music_player_scan(void);

int  music_player_play_index(int index);
int  music_player_toggle_pause(void);
int  music_player_pause_playback(void);
int  music_player_resume(void);
int  music_player_stop(void);
int  music_player_next(void);
int  music_player_prev(void);
int  music_player_seek_permille(uint16_t permille);
int  music_player_seek_ms(uint32_t target_ms);

void              music_player_set_mode(music_play_mode_t mode);
music_play_mode_t music_player_get_mode(void);
const char       *music_player_mode_name(music_play_mode_t mode);

/* Preempt music playback cleanly before alarms or XiaoZhi TTS claim sound0 */
void music_player_preempt(void);

uint32_t music_player_get_track_count(void);
rt_bool_t music_player_get_track(uint32_t index, music_track_info_t *out);
void     music_player_get_status(music_status_t *out);

#ifdef __cplusplus
}
#endif

#endif /* MUSIC_PLAYER_H__ */
