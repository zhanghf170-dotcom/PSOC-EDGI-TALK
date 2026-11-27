#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "music_player.h"
#include "ui_music_lyrics.h"

static bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}

static const char *parse_stamp(const char *p, uint32_t *time_ms)
{
    if (*p++ != '[' || !is_digit(*p)) return NULL;
    unsigned minutes = 0;
    while (is_digit(*p))
    {
        minutes = minutes * 10U + (unsigned)(*p++ - '0');
        if (minutes > 999U) return NULL;
    }
    if (*p++ != ':' || !is_digit(p[0]) || !is_digit(p[1])) return NULL;
    unsigned seconds = (unsigned)(p[0] - '0') * 10U + (unsigned)(p[1] - '0');
    p += 2;
    if (seconds >= 60U) return NULL;

    unsigned fraction = 0;
    if (*p == '.' || *p == ':')
    {
        p++;
        if (!is_digit(*p)) return NULL;
        for (unsigned place = 100; place > 0 && is_digit(*p); place /= 10)
            fraction += (unsigned)(*p++ - '0') * place;
    }
    if (*p++ != ']') return NULL;
    *time_ms = (minutes * 60U + seconds) * 1000U + fraction;
    return p;
}

static void insert_line(music_lyrics_t *lyrics, uint32_t time_ms, const char *text)
{
    if (lyrics->count == MUSIC_LYRIC_LINES) return;
    /* Stable order for repeated timestamps and translation lines. */
    unsigned index = lyrics->count++;
    while (index > 0U && lyrics->lines[index - 1U].time_ms > time_ms)
    {
        lyrics->lines[index] = lyrics->lines[index - 1U];
        index--;
    }
    music_lyric_line_t *line = &lyrics->lines[index];
    line->time_ms = time_ms;
    size_t len = strlen(text);
    if (len >= sizeof(line->text)) len = sizeof(line->text) - 1U;
    while (len > 0U && ((unsigned char)text[len] & 0xc0U) == 0x80U) len--;
    memcpy(line->text, text, len);
    line->text[len] = '\0';
}

void ui_music_lyrics_load(const char *track_path, music_lyrics_t *lyrics)
{
    lyrics->count = 0;
    const char *extension = strrchr(track_path, '.');
    if (!extension) return;
    char path[MUSIC_PATH_MAX];
    snprintf(path, sizeof(path), "%.*s.lrc", (int)(extension - track_path), track_path);
    FILE *file = fopen(path, "rb");
    if (!file) return;

    char buffer[512];
    long offset_ms = 0;
    bool first = true;
    for (unsigned line = 0; line < 512U && fgets(buffer, sizeof(buffer), file); line++)
    {
        char *p = buffer;
        if (first && strncmp(p, "\xef\xbb\xbf", 3) == 0) p += 3;
        first = false;
        if (!strchr(p, '\n') && !feof(file))
        {
            int c;
            do { c = fgetc(file); } while (c != EOF && c != '\n');
            continue;
        }
        p[strcspn(p, "\r\n")] = '\0';
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, "[offset:", 8) == 0)
        {
            char *end;
            long offset = strtol(p + 8, &end, 10);
            if (end != p + 8 && *end == ']' && offset >= -600000 && offset <= 600000)
                offset_ms = offset;
            continue;
        }
        uint32_t times[16];
        unsigned count = 0;
        const char *text = p;
        uint32_t timestamp;
        const char *next;
        while ((next = parse_stamp(text, &timestamp)) != NULL)
        {
            if (count < sizeof(times) / sizeof(times[0])) times[count++] = timestamp;
            text = next;
        }
        while (*text == ' ' || *text == '\t') text++;
        if (*text == '\0') continue;
        for (unsigned i = 0; i < count; i++)
            insert_line(lyrics, times[i], text);
    }
    fclose(file);

    for (unsigned i = 0; i < lyrics->count; i++)
    {
        long shifted = (long)lyrics->lines[i].time_ms - offset_ms;
        lyrics->lines[i].time_ms = shifted > 0 ? (uint32_t)shifted : 0;
    }
}

int ui_music_lyrics_find(const music_lyrics_t *lyrics, uint32_t time_ms)
{
    unsigned low = 0, high = lyrics->count;
    while (low < high)
    {
        unsigned mid = low + (high - low) / 2U;
        if (lyrics->lines[mid].time_ms <= time_ms) low = mid + 1U;
        else high = mid;
    }
    return (int)low - 1;
}
