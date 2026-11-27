#ifndef UI_MUSIC_COVER_H
#define UI_MUSIC_COVER_H

#include <stdint.h>
#include "ui_music_lyrics.h"

#define MUSIC_COVER_SIZE 96U

typedef enum {
    MUSIC_COVER_NONE,
    MUSIC_COVER_LOADING,
    MUSIC_COVER_READY
} music_cover_state_t;

/* Called from the UI thread. A new request supersedes any old result. */
void ui_music_cover_request(const char *track_path, uint32_t mount_gen);
music_cover_state_t ui_music_cover_copy(uint32_t *argb, music_lyrics_t *lyrics);

#endif
