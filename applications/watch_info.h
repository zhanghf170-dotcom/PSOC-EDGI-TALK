#ifndef WATCH_INFO_H
#define WATCH_INFO_H

#include <stdint.h>
#include <time.h>

#define WATCH_INFO_PORT 8889U
#define WATCH_EPOCH_MIN 1577836800UL
#define WATCH_WEATHER_MAX_AGE 1800

typedef enum
{
    WATCH_WEATHER_NONE,
    WATCH_WEATHER_LAN,
    WATCH_WEATHER_USB,
    WATCH_WEATHER_WIFI,
} watch_weather_source_t;

typedef struct
{
    int offset_minutes;
    int weather_code;
    int temperature_tenths;
    int apparent_tenths;
    int humidity;
    int wind_kmh_tenths;
    int city;
    time_t weather_epoch;
    watch_weather_source_t source;
} watch_info_t;

int  watch_info_init(void);
void watch_info_get(watch_info_t *info);
void watch_info_trigger_sync(void);
void watch_info_set_city(int city_id);
int  watch_info_get_selected_city(void);

#endif
