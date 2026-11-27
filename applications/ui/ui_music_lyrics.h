#ifndef UI_MUSIC_LYRICS_H
#define UI_MUSIC_LYRICS_H

#include <stdint.h>

#define MUSIC_LYRIC_LINES 192U
#define MUSIC_LYRIC_TEXT 144U

typedef struct {
    uint32_t time_ms;
    char text[MUSIC_LYRIC_TEXT];
} music_lyric_line_t;

typedef struct {
    unsigned count;
    music_lyric_line_t lines[MUSIC_LYRIC_LINES];
} music_lyrics_t;

void ui_music_lyrics_load(const char *track_path, music_lyrics_t *lyrics);
int ui_music_lyrics_find(const music_lyrics_t *lyrics, uint32_t time_ms);

#endif
