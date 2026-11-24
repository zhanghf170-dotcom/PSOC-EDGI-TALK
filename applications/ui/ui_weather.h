#ifndef UI_WEATHER_H
#define UI_WEATHER_H

#include <stdbool.h>
#include "watch_info.h"

const char *ui_weather_city(int city);
const char *ui_weather_condition(int code);
bool ui_weather_is_fresh(const watch_info_t *info, time_t now);

#endif
