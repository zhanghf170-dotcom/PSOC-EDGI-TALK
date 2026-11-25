#ifndef XIAOZHI_H__
#define XIAOZHI_H__

#include <rtthread.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    XZ_STATE_OFFLINE = 0,
    XZ_STATE_CONNECTING,
    XZ_STATE_ACTIVATING,
    XZ_STATE_IDLE,
    XZ_STATE_LISTENING,
    XZ_STATE_THINKING,
    XZ_STATE_SPEAKING,
    XZ_STATE_SLEEPING,
    XZ_STATE_ERROR
} xz_state_t;

typedef enum
{
    XZ_EMO_IDLE = 0,
    XZ_EMO_LISTENING,
    XZ_EMO_THINKING,
    XZ_EMO_SPEAKING,
    XZ_EMO_WINKING,
    XZ_EMO_SURPRISED,
    XZ_EMO_SAD,
    XZ_EMO_SLEEPY,
    XZ_EMO_COUNT
} xz_emotion_t;

typedef enum
{
    XZ_VOICE_IDLE = 0,
    XZ_VOICE_LISTENING,
    XZ_VOICE_THINKING,
    XZ_VOICE_HAPPY,
    XZ_VOICE_WINKING,
    XZ_VOICE_ALERT,
    XZ_VOICE_OFFLINE,
    XZ_VOICE_SLEEPY,
    XZ_VOICE_BIND,
    XZ_VOICE_COUNT
} xz_voice_clip_t;

typedef enum
{
    XZ_VOICE_MODE_XIAOYI = 0,  /* Use cute zh-CN-XiaoyiNeural voice (LAN live TTS + local clips) */
    XZ_VOICE_MODE_CLOUD  = 1   /* Use XiaoZhi cloud native TTS audio */
} xz_voice_mode_t;

typedef struct
{
    xz_state_t      state;
    xz_emotion_t    emotion;
    xz_voice_mode_t voice_mode;
    rt_bool_t       wifi_ready;
    rt_bool_t       ws_connected;
    rt_bool_t       multi_turn;
    rt_bool_t       mic_active;
    rt_bool_t       spk_active;
    rt_bool_t       tts_bridge_ok;
    uint8_t         mic_level;       /* 0..100 live mic amplitude */
    uint8_t         spk_level;       /* 0..100 live speaker amplitude */
    uint32_t        session_turns;
    uint32_t        ui_rev;
    char            status_text[48];
    char            emotion_name[24];
    char            user_text[160];
    char            assistant_text[256];
    char            activation_code[24];
    char            server_host[96];
} xz_ui_snapshot_t;

/* Public lifecycle & interaction APIs (C-safe for LVGL UI, main.c, alert_manager, wifi_stream) */
int  xiaozhi_init(void);
void xiaozhi_on_wifi_ready(void);
void xiaozhi_trigger_talk(void);
void xiaozhi_stop_conversation(void);
void xiaozhi_reconnect(void);
void xiaozhi_set_multi_turn(rt_bool_t enable);
rt_bool_t xiaozhi_get_multi_turn(void);
void xiaozhi_set_voice_mode(xz_voice_mode_t mode);
xz_voice_mode_t xiaozhi_get_voice_mode(void);
void xiaozhi_set_tts_host(const char *ip);
void xiaozhi_speak_text_xiaoyi(const char *text);
void xiaozhi_set_emotion_name(const char *emotion);
void xiaozhi_set_emotion(xz_emotion_t emo, const char *raw_name);
void xiaozhi_cycle_demo_emotion(void);
void xiaozhi_speak_clip(xz_voice_clip_t clip);
void xiaozhi_greet_once(void);
void xiaozhi_notify_fatigue_alert(float confidence);
void xiaozhi_get_snapshot(xz_ui_snapshot_t *out);
int  xiaozhi_set_server(const char *host, const char *path, const char *token);

/* Shared audio arbiter so fatigue alarms & XiaoZhi TTS never fight over sound0 */
void xiaozhi_audio_lock(void);
void xiaozhi_audio_unlock(void);
void xiaozhi_audio_preempt_for_alarm(void);

/* Internal XiaoZhi audio/protocol hooks */
void xz_audio_send_using_websocket(const uint8_t *data, int len);
void xz_audio_init_system(void);
void xz_mic(int on);
int  xz_mic_is_enabled(void);
void xz_speaker(int on);
int  xz_speaker_is_enabled(void);
void xz_audio_downlink(const uint8_t *data, uint32_t size);
void xz_audio_play_local_voice(xz_voice_clip_t clip);
void xz_audio_stop_local_voice(void);
rt_bool_t xz_audio_is_local_voice_playing(void);
void xz_on_local_voice_finished(void);
int  xz_audio_stream_begin(void);
void xz_audio_stream_feed_opus(const uint8_t *pkt, uint16_t len);
void xz_audio_stream_end(void);
void xz_audio_stream_abort(void);
void xz_tts_net_init(uint16_t port);
rt_bool_t xz_tts_net_is_connected(void);
const char *xz_tts_net_peer_ip(void);
rt_bool_t xz_tts_net_stream_sentence(char *host_buf, size_t host_cap, uint16_t port,
                                     const char *text, volatile rt_bool_t *abort_flag,
                                     rt_bool_t *bridge_ok_out);
void xz_play_wake_sound(void);
uint8_t xz_audio_get_mic_level(void);
uint8_t xz_audio_get_spk_level(void);

/* WebSocket text write helper for MCP server */
int  xz_ws_send_text(const char *text, size_t len);
const char *xz_ws_get_session_id(void);

#ifdef __cplusplus
}
#endif

#endif /* XIAOZHI_H__ */
