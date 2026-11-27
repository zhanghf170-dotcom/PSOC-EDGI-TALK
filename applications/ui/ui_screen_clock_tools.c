/*
 * Time Tools Screen — Calendar (日历), Stopwatch (计时器), Countdown Timer (倒计时)
 *
 * Accessible directly by tapping existing EDGI Watch Face functional areas:
 *   - Clock / Date Card     -> Calendar  (tab 0)
 *   - Seconds Ring          -> Stopwatch (tab 1)
 *   - Top-bar HH:MM:SS pill -> Countdown (tab 2)
 * Also accessible via Apps launcher and Watch Face swipe navigation.
 */

#include <rtthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "ui_manager.h"
#include "watch_info.h"
#include "xiaozhi.h"
#include "sd_assets.h"
#include "src/widgets/calendar/lv_calendar_chinese_private.h"

#define CT_MAX_LAPS  16U
#define CT_CELLS     42U
#define CT_MAX_ANNIVERSARIES 64U
#define CT_ANNIVERSARY_NAME  64U

static ui_chrome_t g_chrome;
static int g_active_tab = 0; /* 0=Calendar, 1=Stopwatch, 2=Countdown */

/* Top tab buttons */
static lv_obj_t *g_tab_btns[3];
static lv_obj_t *g_tab_lbls[3];
static lv_obj_t *g_panes[3];

/* ---------------- Calendar State ---------------- */
static int g_cal_year  = 2026;
static int g_cal_month = 11;
static int g_cal_sel_day = 25;
static int g_today_year = 2026, g_today_month = 11, g_today_day = 25;
static bool g_cal_inited_from_rtc = false;

static lv_obj_t *g_cal_ym_lbl;
static lv_obj_t *g_cal_today_lbl;
static lv_obj_t *g_cal_cells[CT_CELLS];
static lv_obj_t *g_cal_cell_lbls[CT_CELLS];
static lv_obj_t *g_cal_cell_notes[CT_CELLS];
static int       g_cal_cell_days[CT_CELLS];
static lv_obj_t *g_cal_detail_lbl;
static lv_obj_t *g_cal_sub_lbl;
static lv_obj_t *g_cal_event_lbl;

typedef struct {
    uint16_t year; /* Zero repeats every year. */
    uint8_t month;
    uint8_t day;
    char name[CT_ANNIVERSARY_NAME];
} calendar_anniversary_t;

static calendar_anniversary_t g_anniversaries[CT_MAX_ANNIVERSARIES]
    __attribute__((section(".cy_socmem_data")));
static unsigned g_anniversary_count;
static bool g_cal_sd_mounted;
static uint32_t g_cal_sd_gen;

typedef struct {
    uint8_t month;
    uint8_t day;
    const char *name;
} calendar_event_t;

static const calendar_event_t g_solar_events[] = {
    {1, 1, "元旦"}, {2, 14, "情人节"}, {3, 8, "妇女节"},
    {3, 12, "植树节"}, {5, 1, "劳动节"}, {5, 4, "青年节"},
    {6, 1, "儿童节"}, {7, 1, "建党节"}, {8, 1, "建军节"},
    {9, 3, "抗战胜利纪念日"}, {9, 10, "教师节"}, {9, 30, "烈士纪念日"},
    {10, 1, "国庆节"}, {12, 13, "南京大屠杀死难者国家公祭日"},
    {12, 24, "平安夜"}, {12, 25, "圣诞节"},
};

static const calendar_event_t g_lunar_events[] = {
    {1, 1, "春节"}, {1, 15, "元宵节"}, {5, 5, "端午节"},
    {7, 7, "七夕节"}, {8, 15, "中秋节"}, {9, 9, "重阳节"},
    {12, 8, "腊八节"},
};

static const char *const g_lunar_months[] = {
    "正月", "二月", "三月", "四月", "五月", "六月",
    "七月", "八月", "九月", "十月", "冬月", "腊月",
};

static const char *const g_lunar_days[] = {
    "初一", "初二", "初三", "初四", "初五", "初六", "初七", "初八", "初九", "初十",
    "十一", "十二", "十三", "十四", "十五", "十六", "十七", "十八", "十九", "二十",
    "二十一", "二十二", "二十三", "二十四", "二十五", "二十六", "二十七", "二十八", "二十九", "三十",
};

/* ---------------- Stopwatch State ---------------- */
static bool     g_sw_running = false;
static uint32_t g_sw_accum_ms = 0U;
static rt_tick_t g_sw_start_tick = 0U;
static uint32_t g_sw_laps[CT_MAX_LAPS];
static uint32_t g_sw_lap_cnt = 0U;

static lv_obj_t *g_sw_arc;
static lv_obj_t *g_sw_time_lbl;
static lv_obj_t *g_sw_ms_lbl;
static lv_obj_t *g_sw_state_lbl;
static lv_obj_t *g_sw_start_btn;
static lv_obj_t *g_sw_start_lbl;
static lv_obj_t *g_sw_laps_lbl;

/* ---------------- Countdown State ---------------- */
static bool     g_cd_running = false;
static bool     g_cd_finished = false;
static uint32_t g_cd_total_ms = 5U * 60U * 1000U;   /* Default 5 min */
static uint32_t g_cd_remain_ms = 5U * 60U * 1000U;
static rt_tick_t g_cd_end_tick = 0U;

static lv_obj_t *g_cd_arc;
static lv_obj_t *g_cd_time_lbl;
static lv_obj_t *g_cd_state_lbl;
static lv_obj_t *g_cd_start_btn;
static lv_obj_t *g_cd_start_lbl;
static lv_obj_t *g_cd_preset_btns[5];
static lv_obj_t *g_cd_preset_lbls[5];
static const uint32_t g_cd_presets_s[5] = { 60U, 180U, 300U, 600U, 1500U };

/* ---------------- Date Helpers ---------------- */

static bool is_leap_year(int y)
{
    return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

static int days_in_month(int y, int m)
{
    static const int dim[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m < 1 || m > 12) return 30;
    return (m == 2 && is_leap_year(y)) ? 29 : dim[m - 1];
}

/* Returns 0=Sunday .. 6=Saturday */
static int day_of_week(int y, int m, int d)
{
    static const int t[12] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (m < 3) y -= 1;
    return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}

static int day_of_year(int y, int m, int d)
{
    int sum = d;
    for (int i = 1; i < m; i++) sum += days_in_month(y, i);
    return sum;
}

static long civil_to_days(int y, int m, int d)
{
    y -= (m <= 2) ? 1 : 0;
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153U * (unsigned)(m + (m > 2 ? -3 : 9)) + 2U) / 5U + (unsigned)d - 1U;
    unsigned doe = yoe * 365U + yoe / 4U - yoe / 100U + doy;
    return (long)era * 146097L + (long)doe - 719468L;
}

static int read_date_digits(const char *str, unsigned count)
{
    int n = 0;
    for (unsigned i = 0; i < count; i++)
    {
        if (str[i] < '0' || str[i] > '9') return -1;
        n = n * 10 + (str[i] - '0');
    }
    return n;
}

static void cal_reload_anniversaries(void)
{
    g_cal_sd_gen = sdcard_get_mount_gen();
    g_cal_sd_mounted = sdcard_is_mounted();
    g_anniversary_count = 0;
    if (!g_cal_sd_mounted) return;

    FILE *file = fopen(SD_CALENDAR_PATH, "rb");
    if (!file) return;

    char line[CT_ANNIVERSARY_NAME + 16];
    bool first_line = true;
    while (g_anniversary_count < CT_MAX_ANNIVERSARIES && fgets(line, sizeof(line), file))
    {
        char *p = line;
        if (first_line && strncmp(p, "\xef\xbb\xbf", 3) == 0) p += 3;
        first_line = false;
        size_t length = strlen(p);
        if (!strchr(p, '\n') && !feof(file))
        {
            int c;
            do { c = fgetc(file); } while (c != EOF && c != '\n');
            continue;
        }
        while (length && (p[length - 1] == '\r' || p[length - 1] == '\n' ||
                          p[length - 1] == ' ' || p[length - 1] == '\t'))
            p[--length] = '\0';

        char *comma = strchr(p, ',');
        if (!comma || p[0] == '#') continue;
        int year = 0;
        if (comma - p == 10 && p[4] == '-')
        {
            year = read_date_digits(p, 4);
            if (year < 1) continue;
            p += 5;
        }
        if (comma - p != 5 || p[2] != '-') continue;
        int month = read_date_digits(p, 2);
        int day = read_date_digits(p + 3, 2);
        if (month < 1 || month > 12 || day < 1 ||
            day > days_in_month(year ? year : 2000, month)) continue;

        p = comma + 1;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || strlen(p) >= CT_ANNIVERSARY_NAME) continue;

        calendar_anniversary_t *event = &g_anniversaries[g_anniversary_count++];
        event->year = (uint16_t)year;
        event->month = (uint8_t)month;
        event->day = (uint8_t)day;
        strcpy(event->name, p);
    }
    fclose(file);
}

static const char *cal_find_event(const calendar_event_t *events, size_t count, int month, int day)
{
    for (size_t i = 0; i < count; i++)
        if (events[i].month == month && events[i].day == day) return events[i].name;
    return NULL;
}

static const char *cal_solar_event(int year, int month, int day)
{
    /* April dates from the Hong Kong Observatory conversion tables, 2000–2099. */
    static const char qingming[] =
        "4555455544" "5544554455" "4455445544" "5544554455" "4445444544"
        "4544454445" "4445444544" "4544444444" "4444444444" "4444444444";
    if (month == 4 && year >= 2000 && year < 2000 + (int)sizeof(qingming) - 1 &&
        day == qingming[year - 2000] - '0')
        return "清明节";
    if (month == 5 && day >= 8 && day <= 14 && day_of_week(year, month, day) == 0)
        return "母亲节";
    if (month == 6 && day >= 15 && day <= 21 && day_of_week(year, month, day) == 0)
        return "父亲节";
    return cal_find_event(g_solar_events, sizeof(g_solar_events) / sizeof(g_solar_events[0]), month, day);
}

static lv_calendar_chinese_t cal_lunar_date(int year, int month, int day)
{
    /* The LVGL converter may read the preceding year's table before Spring Festival. */
    if (year < 1902 || year > 2099) return (lv_calendar_chinese_t){0};
    lv_calendar_date_t date = { .year = year, .month = month, .day = day };
    return lv_calendar_gregorian_to_chinese(&date);
}

static const char *cal_lunar_event(lv_calendar_chinese_t lunar, int year, int month, int day)
{
    if (lunar.leep_month || lunar.today.day == 0) return NULL;
    if (lunar.today.month == 12 && lunar.today.day >= 29)
    {
        if (++day > days_in_month(year, month))
        {
            day = 1;
            if (++month > 12) { month = 1; year++; }
        }
        lv_calendar_chinese_t next = cal_lunar_date(year, month, day);
        if (next.today.month == 1 && next.today.day == 1) return "除夕";
    }
    return cal_find_event(g_lunar_events, sizeof(g_lunar_events) / sizeof(g_lunar_events[0]),
                          lunar.today.month, lunar.today.day);
}

static bool cal_has_anniversary(int day)
{
    for (unsigned i = 0; i < g_anniversary_count; i++)
    {
        const calendar_anniversary_t *event = &g_anniversaries[i];
        if ((event->year == 0 || event->year == g_cal_year) &&
            event->month == g_cal_month && event->day == day) return true;
    }
    return false;
}

static void cal_append_event(char *text, size_t size, const char *name)
{
    if (!name) return;
    size_t used = strlen(text);
    const char *separator = used ? " · " : "";
    if (strlen(separator) + strlen(name) >= size - used) return;
    strcat(text, separator);
    strcat(text, name);
}

static uint32_t tick_to_ms(rt_tick_t ticks)
{
    return (uint32_t)((uint64_t)ticks * 1000ULL / (uint64_t)RT_TICK_PER_SECOND);
}

static rt_tick_t ms_to_tick(uint32_t ms)
{
    return (rt_tick_t)(((uint64_t)ms * (uint64_t)RT_TICK_PER_SECOND + 999ULL) / 1000ULL);
}

static uint32_t sw_get_elapsed_ms(void)
{
    if (!g_sw_running) return g_sw_accum_ms;
    return g_sw_accum_ms + tick_to_ms(rt_tick_get() - g_sw_start_tick);
}

static uint32_t cd_get_remain_ms(void)
{
    if (!g_cd_running) return g_cd_remain_ms;
    rt_tick_t now = rt_tick_get();
    if ((int32_t)(g_cd_end_tick - now) <= 0) return 0U;
    return tick_to_ms(g_cd_end_tick - now);
}

static void refresh_today_from_rtc(void)
{
    watch_info_t info;
    watch_info_get(&info);
    time_t now = time(NULL);
    if (now >= (time_t)WATCH_EPOCH_MIN)
    {
        time_t local_ep = now + info.offset_minutes * 60;
        struct tm dt;
        if (gmtime_r(&local_ep, &dt) != NULL)
        {
            g_today_year  = dt.tm_year + 1900;
            g_today_month = dt.tm_mon + 1;
            g_today_day   = dt.tm_mday;
            if (!g_cal_inited_from_rtc)
            {
                g_cal_year    = g_today_year;
                g_cal_month   = g_today_month;
                g_cal_sel_day = g_today_day;
                g_cal_inited_from_rtc = true;
            }
        }
    }
}

/* ---------------- Calendar Refresh ---------------- */

static void cal_refresh_view(void)
{
    static const char *const wk[7] = {"日", "一", "二", "三", "四", "五", "六"};
    char buf[96];

    int dim = days_in_month(g_cal_year, g_cal_month);
    if (g_cal_sel_day > dim) g_cal_sel_day = dim;
    if (g_cal_sel_day < 1)   g_cal_sel_day = 1;

    snprintf(buf, sizeof(buf), "%d年 %02d月", g_cal_year, g_cal_month);
    ui_set_label_text(g_cal_ym_lbl, buf);

    int tw = day_of_week(g_today_year, g_today_month, g_today_day);
    snprintf(buf, sizeof(buf), "今日 %d月%d日 星期%s", g_today_month, g_today_day, wk[tw]);
    ui_set_label_text(g_cal_today_lbl, buf);

    int first_wd = day_of_week(g_cal_year, g_cal_month, 1);
    for (unsigned i = 0; i < CT_CELLS; i++)
    {
        int d = (int)i - first_wd + 1;
        if (d < 1 || d > dim)
        {
            g_cal_cell_days[i] = 0;
            ui_set_label_text(g_cal_cell_lbls[i], "");
            ui_set_label_text(g_cal_cell_notes[i], "");
            lv_obj_set_style_bg_opa(g_cal_cells[i], LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(g_cal_cells[i], 0, 0);
            lv_obj_remove_flag(g_cal_cells[i], LV_OBJ_FLAG_CLICKABLE);
        }
        else
        {
            g_cal_cell_days[i] = d;
            snprintf(buf, sizeof(buf), "%d", d);
            ui_set_label_text(g_cal_cell_lbls[i], buf);
            lv_obj_add_flag(g_cal_cells[i], LV_OBJ_FLAG_CLICKABLE);

            lv_calendar_chinese_t lunar = cal_lunar_date(g_cal_year, g_cal_month, d);
            bool anniversary = cal_has_anniversary(d);
            const char *note = cal_lunar_event(lunar, g_cal_year, g_cal_month, d);
            if (!note) note = cal_solar_event(g_cal_year, g_cal_month, d);
            bool holiday = note != NULL;
            if (anniversary) note = "纪念日";
            char lunar_month[16];
            if (!note && lunar.today.day)
            {
                if (lunar.today.day == 1)
                {
                    snprintf(lunar_month, sizeof(lunar_month), "%s%s", lunar.leep_month ? "闰" : "",
                             g_lunar_months[lunar.today.month - 1]);
                    note = lunar_month;
                }
                else note = g_lunar_days[lunar.today.day - 1];
            }
            ui_set_label_text(g_cal_cell_notes[i], note ? note : "");

            bool is_today = (g_cal_year == g_today_year && g_cal_month == g_today_month && d == g_today_day);
            bool is_sel   = (d == g_cal_sel_day);
            int  col      = (int)(i % 7U);
            bool weekend  = (col == 0 || col == 6);
            lv_obj_set_style_text_color(g_cal_cell_notes[i], is_today ? lv_color_hex(0x102A43)
                                      : anniversary ? UI_COL_GEMINI_PURPLE
                                      : holiday ? UI_COL_AMBER : UI_COL_MUTED, 0);

            if (is_today)
            {
                lv_obj_set_style_bg_color(g_cal_cells[i], UI_COL_CYAN, 0);
                lv_obj_set_style_bg_opa(g_cal_cells[i], LV_OPA_COVER, 0);
                lv_obj_set_style_border_width(g_cal_cells[i], is_sel ? 2 : 0, 0);
                lv_obj_set_style_border_color(g_cal_cells[i], UI_COL_TEXT, 0);
                lv_obj_set_style_text_color(g_cal_cell_lbls[i], lv_color_hex(0x06101F), 0);
            }
            else if (is_sel)
            {
                lv_obj_set_style_bg_color(g_cal_cells[i], lv_color_hex(0x282159), 0);
                lv_obj_set_style_bg_opa(g_cal_cells[i], LV_OPA_COVER, 0);
                lv_obj_set_style_border_width(g_cal_cells[i], 2, 0);
                lv_obj_set_style_border_color(g_cal_cells[i], UI_COL_GEMINI_PURPLE, 0);
                lv_obj_set_style_text_color(g_cal_cell_lbls[i], UI_COL_TEXT, 0);
            }
            else
            {
                lv_obj_set_style_bg_opa(g_cal_cells[i], LV_OPA_TRANSP, 0);
                lv_obj_set_style_border_width(g_cal_cells[i], 0, 0);
                lv_obj_set_style_text_color(g_cal_cell_lbls[i],
                                            weekend ? lv_color_hex(0xFCA5A5) : UI_COL_TEXT, 0);
            }
        }
    }

    int sw = day_of_week(g_cal_year, g_cal_month, g_cal_sel_day);
    snprintf(buf, sizeof(buf), "%d年%d月%d日  星期%s",
             g_cal_year, g_cal_month, g_cal_sel_day, wk[sw]);
    ui_set_label_text(g_cal_detail_lbl, buf);

    long diff = civil_to_days(g_cal_year, g_cal_month, g_cal_sel_day) -
                civil_to_days(g_today_year, g_today_month, g_today_day);
    int doy = day_of_year(g_cal_year, g_cal_month, g_cal_sel_day);
    lv_calendar_chinese_t lunar = cal_lunar_date(g_cal_year, g_cal_month, g_cal_sel_day);
    int offset;
    if (lunar.today.day)
        offset = snprintf(buf, sizeof(buf), "农历%s%s%s", lunar.leep_month ? "闰" : "",
                          g_lunar_months[lunar.today.month - 1], g_lunar_days[lunar.today.day - 1]);
    else
        offset = snprintf(buf, sizeof(buf), "本年第 %d 天", doy);
    if (diff == 0) snprintf(buf + offset, sizeof(buf) - offset, "  ·  今天");
    else snprintf(buf + offset, sizeof(buf) - offset, "  ·  %ld 天%s",
                  diff > 0 ? diff : -diff, diff > 0 ? "后" : "前");
    ui_set_label_text(g_cal_sub_lbl, buf);

    char events[256] = "";
    cal_append_event(events, sizeof(events), cal_solar_event(g_cal_year, g_cal_month, g_cal_sel_day));
    cal_append_event(events, sizeof(events), cal_lunar_event(lunar, g_cal_year, g_cal_month, g_cal_sel_day));
    for (unsigned i = 0; i < g_anniversary_count; i++)
    {
        const calendar_anniversary_t *event = &g_anniversaries[i];
        if ((event->year == 0 || event->year == g_cal_year) &&
            event->month == g_cal_month && event->day == g_cal_sel_day)
            cal_append_event(events, sizeof(events), event->name);
    }
    ui_set_label_text(g_cal_event_lbl, events);
}

/* ---------------- Stopwatch Refresh ---------------- */

static void sw_refresh_laps_text(void)
{
    if (g_sw_lap_cnt == 0U)
    {
        ui_set_label_text(g_sw_laps_lbl, "暂无计次记录，点击「计次」保存分段用时");
        return;
    }
    char out[512];
    size_t pos = 0U;
    out[0] = '\0';
    for (int i = (int)g_sw_lap_cnt - 1; i >= 0; i--)
    {
        uint32_t t = g_sw_laps[i];
        uint32_t prev = (i > 0) ? g_sw_laps[i - 1] : 0U;
        uint32_t d = (t >= prev) ? (t - prev) : 0U;
        int n = rt_snprintf(out + pos, sizeof(out) - pos,
                            "#%02d   %02lu:%02lu.%02lu   (+%02lu:%02lu.%02lu)%s",
                            i + 1,
                            (unsigned long)(t / 60000U), (unsigned long)((t / 1000U) % 60U),
                            (unsigned long)((t / 10U) % 100U),
                            (unsigned long)(d / 60000U), (unsigned long)((d / 1000U) % 60U),
                            (unsigned long)((d / 10U) % 100U),
                            i > 0 ? "\n" : "");
        if (n <= 0 || pos + (size_t)n >= sizeof(out)) break;
        pos += (size_t)n;
    }
    ui_set_label_text(g_sw_laps_lbl, out);
}

static void sw_refresh_view(void)
{
    uint32_t ms = sw_get_elapsed_ms();
    char buf[32];
    snprintf(buf, sizeof(buf), "%02lu:%02lu",
             (unsigned long)(ms / 60000U), (unsigned long)((ms / 1000U) % 60U));
    ui_set_label_text(g_sw_time_lbl, buf);
    snprintf(buf, sizeof(buf), ".%02lu", (unsigned long)((ms / 10U) % 100U));
    ui_set_label_text(g_sw_ms_lbl, buf);

    int32_t arc_val = (int32_t)((ms % 60000U) / 100U); /* 0..600 smooth 0.1s resolution */
    lv_arc_set_value(g_sw_arc, arc_val);

    if (g_sw_running)
    {
        ui_set_label_text(g_sw_state_lbl, "计时中");
        lv_obj_set_style_text_color(g_sw_state_lbl, UI_COL_GREEN, 0);
        ui_set_label_text(g_sw_start_lbl, LV_SYMBOL_PAUSE " 暂停");
        lv_obj_set_style_bg_color(g_sw_start_btn, UI_COL_AMBER, 0);
    }
    else if (g_sw_accum_ms > 0U)
    {
        ui_set_label_text(g_sw_state_lbl, "已暂停");
        lv_obj_set_style_text_color(g_sw_state_lbl, UI_COL_AMBER, 0);
        ui_set_label_text(g_sw_start_lbl, LV_SYMBOL_PLAY " 继续");
        lv_obj_set_style_bg_color(g_sw_start_btn, UI_COL_CYAN, 0);
    }
    else
    {
        ui_set_label_text(g_sw_state_lbl, "准备就绪");
        lv_obj_set_style_text_color(g_sw_state_lbl, UI_COL_MUTED, 0);
        ui_set_label_text(g_sw_start_lbl, LV_SYMBOL_PLAY " 开始");
        lv_obj_set_style_bg_color(g_sw_start_btn, UI_COL_CYAN, 0);
    }
}

/* ---------------- Countdown Refresh ---------------- */

static void cd_refresh_view(void)
{
    uint32_t rem = cd_get_remain_ms();
    uint32_t secs = (rem + 999U) / 1000U;
    if (!g_cd_running && !g_cd_finished)
        secs = g_cd_remain_ms / 1000U;
    if (g_cd_finished)
        secs = 0U;

    char buf[32];
    snprintf(buf, sizeof(buf), "%02lu:%02lu",
             (unsigned long)(secs / 60U), (unsigned long)(secs % 60U));
    ui_set_label_text(g_cd_time_lbl, buf);

    int32_t pct = (g_cd_total_ms > 0U) ? (int32_t)(((uint64_t)rem * 600ULL) / g_cd_total_ms) : 0;
    if (pct > 600) pct = 600;
    if (g_cd_finished) pct = 0;
    lv_arc_set_value(g_cd_arc, pct);

    lv_color_t arc_col = g_cd_finished ? UI_COL_RED : (pct < 120 ? UI_COL_AMBER : UI_COL_CYAN);
    lv_obj_set_style_arc_color(g_cd_arc, arc_col, LV_PART_INDICATOR);

    if (g_cd_finished)
    {
        ui_set_label_text(g_cd_state_lbl, "倒计时结束！");
        lv_obj_set_style_text_color(g_cd_state_lbl, UI_COL_RED, 0);
        ui_set_label_text(g_cd_start_lbl, LV_SYMBOL_REFRESH " 重新开始");
        lv_obj_set_style_bg_color(g_cd_start_btn, UI_COL_GREEN, 0);
    }
    else if (g_cd_running)
    {
        ui_set_label_text(g_cd_state_lbl, "倒计时中");
        lv_obj_set_style_text_color(g_cd_state_lbl, UI_COL_GREEN, 0);
        ui_set_label_text(g_cd_start_lbl, LV_SYMBOL_PAUSE " 暂停");
        lv_obj_set_style_bg_color(g_cd_start_btn, UI_COL_AMBER, 0);
    }
    else
    {
        ui_set_label_text(g_cd_state_lbl, (g_cd_remain_ms < g_cd_total_ms) ? "已暂停" : "准备就绪");
        lv_obj_set_style_text_color(g_cd_state_lbl, UI_COL_MUTED, 0);
        ui_set_label_text(g_cd_start_lbl, LV_SYMBOL_PLAY " 开始倒计时");
        lv_obj_set_style_bg_color(g_cd_start_btn, UI_COL_CYAN, 0);
    }

    for (unsigned i = 0; i < 5U; i++)
    {
        bool sel = (g_cd_total_ms == g_cd_presets_s[i] * 1000U);
        lv_obj_set_style_bg_color(g_cd_preset_btns[i], sel ? lv_color_hex(0x163459) : UI_COL_CARD_HI, 0);
        lv_obj_set_style_border_color(g_cd_preset_btns[i], sel ? UI_COL_CYAN : UI_COL_BORDER, 0);
        lv_obj_set_style_text_color(g_cd_preset_lbls[i], sel ? UI_COL_CYAN : UI_COL_TEXT, 0);
    }
}

/* ---------------- Tab Switching ---------------- */

static void apply_tab_ui(void)
{
    for (int i = 0; i < 3; i++)
    {
        bool active = (i == g_active_tab);
        if (g_panes[i] != NULL)
        {
            if (active) lv_obj_remove_flag(g_panes[i], LV_OBJ_FLAG_HIDDEN);
            else        lv_obj_add_flag(g_panes[i], LV_OBJ_FLAG_HIDDEN);
        }
        if (g_tab_btns[i] != NULL && g_tab_lbls[i] != NULL)
        {
            lv_obj_set_style_bg_color(g_tab_btns[i], active ? lv_color_hex(0x163459) : UI_COL_CARD, 0);
            lv_obj_set_style_border_color(g_tab_btns[i], active ? UI_COL_CYAN : UI_COL_BORDER, 0);
            lv_obj_set_style_text_color(g_tab_lbls[i], active ? UI_COL_CYAN : UI_COL_MUTED, 0);
        }
    }
}

void ui_screen_clock_tools_open_tab(int tab)
{
    if (tab < 0 || tab > 2) tab = 0;
    g_active_tab = tab;
    if (g_panes[0] != NULL)
    {
        apply_tab_ui();
        if (tab == 0)
        {
            cal_reload_anniversaries();
            refresh_today_from_rtc();
            cal_refresh_view();
        }
        ui_screen_clock_tools_update();
    }
}

/* ---------------- Event Handlers ---------------- */

static void tab_btn_cb(lv_event_t *e)
{
    ui_screen_clock_tools_open_tab((int)(intptr_t)lv_event_get_user_data(e));
}

static void cal_move_month(int step)
{
    int month = (g_cal_year - 1) * 12 + g_cal_month - 1 + step;
    if (month < 0 || month >= 9999 * 12) return;
    g_cal_year = month / 12 + 1;
    g_cal_month = month % 12 + 1;
    cal_refresh_view();
}

bool ui_screen_clock_tools_handle_swipe(lv_dir_t dir)
{
    if (g_active_tab != 0 || (dir != LV_DIR_LEFT && dir != LV_DIR_RIGHT)) return false;
    cal_move_month(dir == LV_DIR_LEFT ? 1 : -1);
    return true;
}

static void cal_month_cb(lv_event_t *e)
{
    cal_move_month((int)(intptr_t)lv_event_get_user_data(e));
}

static void cal_today_cb(lv_event_t *e)
{
    (void)e;
    refresh_today_from_rtc();
    g_cal_year    = g_today_year;
    g_cal_month   = g_today_month;
    g_cal_sel_day = g_today_day;
    cal_refresh_view();
}

static void cal_cell_cb(lv_event_t *e)
{
    unsigned idx = (unsigned)(uintptr_t)lv_event_get_user_data(e);
    if (idx < CT_CELLS && g_cal_cell_days[idx] > 0)
    {
        g_cal_sel_day = g_cal_cell_days[idx];
        cal_refresh_view();
    }
}

static void sw_start_cb(lv_event_t *e)
{
    (void)e;
    if (g_sw_running)
    {
        g_sw_accum_ms = sw_get_elapsed_ms();
        g_sw_running  = false;
    }
    else
    {
        g_sw_start_tick = rt_tick_get();
        g_sw_running    = true;
    }
    sw_refresh_view();
}

static void sw_lap_cb(lv_event_t *e)
{
    (void)e;
    uint32_t ms = sw_get_elapsed_ms();
    if (ms == 0U) return;
    if (g_sw_lap_cnt < CT_MAX_LAPS)
    {
        g_sw_laps[g_sw_lap_cnt++] = ms;
    }
    else
    {
        rt_memmove(&g_sw_laps[0], &g_sw_laps[1], (CT_MAX_LAPS - 1U) * sizeof(uint32_t));
        g_sw_laps[CT_MAX_LAPS - 1U] = ms;
    }
    sw_refresh_laps_text();
}

static void sw_reset_cb(lv_event_t *e)
{
    (void)e;
    g_sw_running  = false;
    g_sw_accum_ms = 0U;
    g_sw_lap_cnt  = 0U;
    sw_refresh_laps_text();
    sw_refresh_view();
}

static void cd_start_cb(lv_event_t *e)
{
    (void)e;
    if (g_cd_finished)
    {
        g_cd_finished  = false;
        g_cd_remain_ms = g_cd_total_ms;
        g_cd_end_tick  = rt_tick_get() + ms_to_tick(g_cd_remain_ms);
        g_cd_running   = (g_cd_remain_ms > 0U);
    }
    else if (g_cd_running)
    {
        g_cd_remain_ms = cd_get_remain_ms();
        g_cd_running   = false;
    }
    else
    {
        if (g_cd_remain_ms == 0U) g_cd_remain_ms = g_cd_total_ms;
        if (g_cd_remain_ms > 0U)
        {
            g_cd_end_tick = rt_tick_get() + ms_to_tick(g_cd_remain_ms);
            g_cd_running  = true;
        }
    }
    cd_refresh_view();
}

static void cd_reset_cb(lv_event_t *e)
{
    (void)e;
    g_cd_running   = false;
    g_cd_finished  = false;
    g_cd_remain_ms = g_cd_total_ms;
    cd_refresh_view();
}

static void cd_preset_cb(lv_event_t *e)
{
    unsigned idx = (unsigned)(uintptr_t)lv_event_get_user_data(e);
    if (idx >= 5U) return;
    g_cd_running   = false;
    g_cd_finished  = false;
    g_cd_total_ms  = g_cd_presets_s[idx] * 1000U;
    g_cd_remain_ms = g_cd_total_ms;
    cd_refresh_view();
}

static void cd_adjust_cb(lv_event_t *e)
{
    int32_t delta_s = (int32_t)(intptr_t)lv_event_get_user_data(e);
    if (g_cd_running) return;
    g_cd_finished = false;
    int32_t cur_s = (int32_t)(g_cd_remain_ms / 1000U) + delta_s;
    if (cur_s < 10)   cur_s = 10;
    if (cur_s > 5990) cur_s = 5990;
    g_cd_total_ms  = (uint32_t)cur_s * 1000U;
    g_cd_remain_ms = g_cd_total_ms;
    cd_refresh_view();
}

/* ---------------- Widget Helpers ---------------- */

static lv_obj_t *ct_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t col, const char *txt)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt ? txt : "");
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, col, 0);
    return l;
}

static lv_obj_t *ct_card(lv_obj_t *parent, int32_t w, int32_t h)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_add_style(c, &g_st_card, 0);
    lv_obj_set_size(c, w, h);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

/* ---------------- Build Panes ---------------- */

static lv_obj_t *build_calendar_pane(lv_obj_t *body)
{
    const lv_font_t *f_full  = ui_font_zh_full();
    const lv_font_t *f_small = ui_font_zh_small();

    lv_obj_t *pane = lv_obj_create(body);
    lv_obj_remove_style_all(pane);
    lv_obj_set_size(pane, 460, 616);
    lv_obj_remove_flag(pane, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(pane, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(pane, 10, 0);

    /* Month Header Card (460 x 76) */
    lv_obj_t *hdr = ct_card(pane, 460, 76);
    lv_obj_set_style_pad_hor(hdr, 10, 0);
    lv_obj_set_style_pad_ver(hdr, 8, 0);

    lv_obj_t *b_prev = lv_button_create(hdr);
    lv_obj_add_style(b_prev, &g_st_btn_ghost, 0);
    lv_obj_set_size(b_prev, 68, 40);
    lv_obj_align(b_prev, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_add_event_cb(b_prev, cal_month_cb, LV_EVENT_CLICKED, (void *)(intptr_t)-1);
    lv_obj_center(ct_label(b_prev, f_small, UI_COL_TEXT, LV_SYMBOL_LEFT));

    g_cal_ym_lbl = ct_label(hdr, f_full, UI_COL_CYAN, "2026年 11月");
    lv_obj_align(g_cal_ym_lbl, LV_ALIGN_TOP_MID, -16, 2);

    g_cal_today_lbl = ct_label(hdr, f_small, UI_COL_MUTED, "今日 11月25日 星期三");
    lv_obj_align(g_cal_today_lbl, LV_ALIGN_BOTTOM_MID, -16, -2);

    lv_obj_t *b_today = lv_button_create(hdr);
    lv_obj_add_style(b_today, &g_st_btn_ghost, 0);
    lv_obj_set_size(b_today, 62, 40);
    lv_obj_set_style_border_color(b_today, UI_COL_CYAN, 0);
    lv_obj_align(b_today, LV_ALIGN_RIGHT_MID, -76, 0);
    lv_obj_add_event_cb(b_today, cal_today_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_center(ct_label(b_today, f_small, UI_COL_CYAN, "今天"));

    lv_obj_t *b_next = lv_button_create(hdr);
    lv_obj_add_style(b_next, &g_st_btn_ghost, 0);
    lv_obj_set_size(b_next, 68, 40);
    lv_obj_align(b_next, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(b_next, cal_month_cb, LV_EVENT_CLICKED, (void *)(intptr_t)1);
    lv_obj_center(ct_label(b_next, f_small, UI_COL_TEXT, LV_SYMBOL_RIGHT));

    lv_obj_t *gcard = ct_card(pane, 460, 416);
    lv_obj_set_style_pad_all(gcard, 10, 0);
    lv_obj_set_flex_flow(gcard, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(gcard, 6, 0);

    lv_obj_t *wrow = lv_obj_create(gcard);
    lv_obj_remove_style_all(wrow);
    lv_obj_set_size(wrow, 436, 30);
    lv_obj_remove_flag(wrow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(wrow, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(wrow, 4, 0);
    static const char *const wnames[7] = {"日", "一", "二", "三", "四", "五", "六"};
    for (int i = 0; i < 7; i++)
    {
        lv_obj_t *wc = lv_obj_create(wrow);
        lv_obj_remove_style_all(wc);
        lv_obj_set_size(wc, 58, 28);
        lv_obj_center(ct_label(wc, f_small, (i == 0 || i == 6) ? lv_color_hex(0xFCA5A5) : UI_COL_MUTED, wnames[i]));
    }

    lv_obj_t *grid = lv_obj_create(gcard);
    lv_obj_remove_style_all(grid);
    lv_obj_set_size(grid, 436, 356);
    lv_obj_remove_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(grid, 4, 0);
    lv_obj_set_style_pad_row(grid, 4, 0);

    for (unsigned i = 0; i < CT_CELLS; i++)
    {
        lv_obj_t *cell = lv_obj_create(grid);
        lv_obj_remove_style_all(cell);
        lv_obj_set_size(cell, 58, 56);
        lv_obj_set_style_radius(cell, 14, 0);
        lv_obj_remove_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(cell, cal_cell_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)i);

        g_cal_cells[i] = cell;
        g_cal_cell_lbls[i] = ct_label(cell, &lv_font_montserrat_20, UI_COL_TEXT, "");
        lv_obj_align(g_cal_cell_lbls[i], LV_ALIGN_TOP_MID, 0, 5);
        g_cal_cell_notes[i] = ct_label(cell, f_small, UI_COL_MUTED, "");
        lv_obj_set_width(g_cal_cell_notes[i], 56);
        lv_obj_set_style_text_align(g_cal_cell_notes[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(g_cal_cell_notes[i], LV_LABEL_LONG_DOT);
        lv_obj_align(g_cal_cell_notes[i], LV_ALIGN_BOTTOM_MID, 0, -5);
    }

    lv_obj_t *dcard = ct_card(pane, 460, 104);
    lv_obj_set_style_pad_hor(dcard, 14, 0);
    lv_obj_set_style_pad_ver(dcard, 10, 0);
    g_cal_detail_lbl = ct_label(dcard, f_full, UI_COL_TEXT, "2026年11月25日 星期三");
    lv_obj_align(g_cal_detail_lbl, LV_ALIGN_TOP_LEFT, 0, 2);
    g_cal_sub_lbl = ct_label(dcard, f_small, UI_COL_MUTED, "");
    lv_obj_align(g_cal_sub_lbl, LV_ALIGN_TOP_LEFT, 0, 32);
    g_cal_event_lbl = ct_label(dcard, f_small, UI_COL_AMBER, "");
    lv_obj_set_width(g_cal_event_lbl, 430);
    lv_label_set_long_mode(g_cal_event_lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_align(g_cal_event_lbl, LV_ALIGN_TOP_LEFT, 0, 58);

    return pane;
}

static lv_obj_t *build_stopwatch_pane(lv_obj_t *body)
{
    const lv_font_t *f_full  = ui_font_zh_full();
    const lv_font_t *f_small = ui_font_zh_small();

    lv_obj_t *pane = lv_obj_create(body);
    lv_obj_remove_style_all(pane);
    lv_obj_set_size(pane, 460, 616);
    lv_obj_remove_flag(pane, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(pane, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(pane, 10, 0);

    /* Stopwatch Arc Hero Card (460 x 272) */
    lv_obj_t *hero = ct_card(pane, 460, 272);
    g_sw_arc = lv_arc_create(hero);
    lv_obj_set_size(g_sw_arc, 224, 224);
    lv_obj_center(g_sw_arc);
    lv_arc_set_bg_angles(g_sw_arc, 0, 360);
    lv_arc_set_rotation(g_sw_arc, 270);
    lv_arc_set_range(g_sw_arc, 0, 600);
    lv_arc_set_value(g_sw_arc, 0);
    lv_obj_remove_style(g_sw_arc, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(g_sw_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(g_sw_arc, 10, LV_PART_MAIN);
    lv_obj_set_style_arc_width(g_sw_arc, 10, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(g_sw_arc, lv_color_hex(0x1E293B), LV_PART_MAIN);
    lv_obj_set_style_arc_color(g_sw_arc, UI_COL_CYAN, LV_PART_INDICATOR);

    g_sw_time_lbl = ct_label(hero, &lv_font_montserrat_48, UI_COL_TEXT, "00:00");
    lv_obj_align(g_sw_time_lbl, LV_ALIGN_CENTER, 0, -14);

    g_sw_ms_lbl = ct_label(hero, &lv_font_montserrat_24, UI_COL_CYAN, ".00");
    lv_obj_align(g_sw_ms_lbl, LV_ALIGN_CENTER, 0, 28);

    g_sw_state_lbl = ct_label(hero, f_small, UI_COL_MUTED, "准备就绪");
    lv_obj_align(g_sw_state_lbl, LV_ALIGN_BOTTOM_MID, 0, -2);

    /* Control Row (460 x 54) */
    lv_obj_t *ctrl = lv_obj_create(pane);
    lv_obj_remove_style_all(ctrl);
    lv_obj_set_size(ctrl, 460, 54);
    lv_obj_set_flex_flow(ctrl, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(ctrl, 10, 0);

    g_sw_start_btn = lv_button_create(ctrl);
    lv_obj_add_style(g_sw_start_btn, &g_st_btn_primary, 0);
    lv_obj_set_flex_grow(g_sw_start_btn, 2);
    lv_obj_set_height(g_sw_start_btn, 52);
    lv_obj_add_event_cb(g_sw_start_btn, sw_start_cb, LV_EVENT_CLICKED, NULL);
    g_sw_start_lbl = ct_label(g_sw_start_btn, f_full, lv_color_hex(0x06101F), LV_SYMBOL_PLAY " 开始");
    lv_obj_center(g_sw_start_lbl);

    lv_obj_t *b_lap = lv_button_create(ctrl);
    lv_obj_add_style(b_lap, &g_st_btn_ghost, 0);
    lv_obj_set_style_border_color(b_lap, UI_COL_GEMINI_PURPLE, 0);
    lv_obj_set_flex_grow(b_lap, 1);
    lv_obj_set_height(b_lap, 52);
    lv_obj_add_event_cb(b_lap, sw_lap_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_center(ct_label(b_lap, f_full, UI_COL_GEMINI_PURPLE, "计次"));

    lv_obj_t *b_rst = lv_button_create(ctrl);
    lv_obj_add_style(b_rst, &g_st_btn_ghost, 0);
    lv_obj_set_flex_grow(b_rst, 1);
    lv_obj_set_height(b_rst, 52);
    lv_obj_add_event_cb(b_rst, sw_reset_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_center(ct_label(b_rst, f_full, UI_COL_TEXT, "复位"));

    /* Lap History Card (460 x 268, scrollable) */
    lv_obj_t *lcard = ct_card(pane, 460, 268);
    lv_obj_add_flag(lcard, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(lcard, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_flex_flow(lcard, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(lcard, 6, 0);

    ct_label(lcard, f_small, UI_COL_CYAN, "分段计次记录");
    g_sw_laps_lbl = ct_label(lcard, f_small, UI_COL_MUTED, "暂无计次记录，点击「计次」保存分段用时");
    lv_obj_set_width(g_sw_laps_lbl, 430);
    lv_label_set_long_mode(g_sw_laps_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(g_sw_laps_lbl, 6, 0);

    return pane;
}

static lv_obj_t *build_countdown_pane(lv_obj_t *body)
{
    const lv_font_t *f_full  = ui_font_zh_full();
    const lv_font_t *f_small = ui_font_zh_small();

    lv_obj_t *pane = lv_obj_create(body);
    lv_obj_remove_style_all(pane);
    lv_obj_set_size(pane, 460, 616);
    lv_obj_remove_flag(pane, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(pane, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(pane, 10, 0);

    /* Countdown Arc Hero Card (460 x 290) */
    lv_obj_t *hero = ct_card(pane, 460, 290);
    g_cd_arc = lv_arc_create(hero);
    lv_obj_set_size(g_cd_arc, 232, 232);
    lv_obj_center(g_cd_arc);
    lv_arc_set_bg_angles(g_cd_arc, 0, 360);
    lv_arc_set_rotation(g_cd_arc, 270);
    lv_arc_set_range(g_cd_arc, 0, 600);
    lv_arc_set_value(g_cd_arc, 600);
    lv_obj_remove_style(g_cd_arc, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(g_cd_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(g_cd_arc, 12, LV_PART_MAIN);
    lv_obj_set_style_arc_width(g_cd_arc, 12, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(g_cd_arc, lv_color_hex(0x1E293B), LV_PART_MAIN);
    lv_obj_set_style_arc_color(g_cd_arc, UI_COL_CYAN, LV_PART_INDICATOR);

    g_cd_time_lbl = ct_label(hero, &lv_font_montserrat_48, UI_COL_TEXT, "05:00");
    lv_obj_align(g_cd_time_lbl, LV_ALIGN_CENTER, 0, -10);

    g_cd_state_lbl = ct_label(hero, f_small, UI_COL_MUTED, "准备就绪");
    lv_obj_align(g_cd_state_lbl, LV_ALIGN_CENTER, 0, 36);

    /* Quick Preset Row (460 x 88 card) */
    lv_obj_t *pcard = ct_card(pane, 460, 88);
    lv_obj_set_style_pad_all(pcard, 10, 0);
    lv_obj_set_flex_flow(pcard, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(pcard, 6, 0);
    ct_label(pcard, f_small, UI_COL_MUTED, "快捷预设时长");

    lv_obj_t *prow = lv_obj_create(pcard);
    lv_obj_remove_style_all(prow);
    lv_obj_set_size(prow, 436, 40);
    lv_obj_set_flex_flow(prow, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(prow, 8, 0);

    static const char *const pnames[5] = {"1分", "3分", "5分", "10分", "25分"};
    for (unsigned i = 0; i < 5U; i++)
    {
        g_cd_preset_btns[i] = lv_button_create(prow);
        lv_obj_add_style(g_cd_preset_btns[i], &g_st_btn_ghost, 0);
        lv_obj_set_flex_grow(g_cd_preset_btns[i], 1);
        lv_obj_set_height(g_cd_preset_btns[i], 38);
        lv_obj_add_event_cb(g_cd_preset_btns[i], cd_preset_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
        g_cd_preset_lbls[i] = ct_label(g_cd_preset_btns[i], f_small, UI_COL_TEXT, pnames[i]);
        lv_obj_center(g_cd_preset_lbls[i]);
    }

    /* Fine Adjust Row (460 x 86 card) */
    lv_obj_t *acard = ct_card(pane, 460, 86);
    lv_obj_set_style_pad_all(acard, 10, 0);
    lv_obj_set_flex_flow(acard, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(acard, 6, 0);
    ct_label(acard, f_small, UI_COL_MUTED, "微调时长 (暂停状态下可调)");

    lv_obj_t *arow = lv_obj_create(acard);
    lv_obj_remove_style_all(arow);
    lv_obj_set_size(arow, 436, 40);
    lv_obj_set_flex_flow(arow, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(arow, 8, 0);

    static const struct { const char *lbl; int32_t delta; } adj[4] = {
        {"-1分", -60}, {"-10秒", -10}, {"+10秒", 10}, {"+1分", 60}
    };
    for (unsigned i = 0; i < 4U; i++)
    {
        lv_obj_t *b = lv_button_create(arow);
        lv_obj_add_style(b, &g_st_btn_ghost, 0);
        lv_obj_set_flex_grow(b, 1);
        lv_obj_set_height(b, 38);
        lv_obj_add_event_cb(b, cd_adjust_cb, LV_EVENT_CLICKED, (void *)(intptr_t)adj[i].delta);
        lv_obj_center(ct_label(b, f_small, UI_COL_TEXT, adj[i].lbl));
    }

    /* Start / Reset Control Row (460 x 56) */
    lv_obj_t *ctrl = lv_obj_create(pane);
    lv_obj_remove_style_all(ctrl);
    lv_obj_set_size(ctrl, 460, 56);
    lv_obj_set_flex_flow(ctrl, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(ctrl, 10, 0);

    g_cd_start_btn = lv_button_create(ctrl);
    lv_obj_add_style(g_cd_start_btn, &g_st_btn_primary, 0);
    lv_obj_set_flex_grow(g_cd_start_btn, 2);
    lv_obj_set_height(g_cd_start_btn, 54);
    lv_obj_add_event_cb(g_cd_start_btn, cd_start_cb, LV_EVENT_CLICKED, NULL);
    g_cd_start_lbl = ct_label(g_cd_start_btn, f_full, lv_color_hex(0x06101F), LV_SYMBOL_PLAY " 开始倒计时");
    lv_obj_center(g_cd_start_lbl);

    lv_obj_t *b_rst = lv_button_create(ctrl);
    lv_obj_add_style(b_rst, &g_st_btn_ghost, 0);
    lv_obj_set_flex_grow(b_rst, 1);
    lv_obj_set_height(b_rst, 54);
    lv_obj_add_event_cb(b_rst, cd_reset_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_center(ct_label(b_rst, f_full, UI_COL_TEXT, "重置"));

    return pane;
}

/* ---------------- Public Lifecycle APIs ---------------- */

lv_obj_t *ui_screen_clock_tools_create(void)
{
    const lv_font_t *f_small = ui_font_zh_small();

    lv_obj_t *scr = lv_obj_create(NULL);
    ui_create_chrome(scr, "日历 · 计时 · 倒计时", UI_SCR_CLOCK_TOOLS, &g_chrome);

    lv_obj_t *body = g_chrome.body;
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(body, 10, 0);
    lv_obj_set_style_pad_row(body, 10, 0);

    /* Top Segmented Tab Bar (460 x 42) */
    lv_obj_t *tabs = lv_obj_create(body);
    lv_obj_remove_style_all(tabs);
    lv_obj_set_size(tabs, 460, 42);
    lv_obj_remove_flag(tabs, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(tabs, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(tabs, 8, 0);

    static const char *const tnames[3] = {
        LV_SYMBOL_LIST " 日历",
        LV_SYMBOL_PLAY " 计时器",
        LV_SYMBOL_BELL " 倒计时"
    };
    for (int i = 0; i < 3; i++)
    {
        g_tab_btns[i] = lv_button_create(tabs);
        lv_obj_add_style(g_tab_btns[i], &g_st_btn_ghost, 0);
        lv_obj_set_flex_grow(g_tab_btns[i], 1);
        lv_obj_set_height(g_tab_btns[i], 40);
        lv_obj_set_style_radius(g_tab_btns[i], 20, 0);
        lv_obj_add_event_cb(g_tab_btns[i], tab_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        g_tab_lbls[i] = ct_label(g_tab_btns[i], f_small, UI_COL_MUTED, tnames[i]);
        lv_obj_center(g_tab_lbls[i]);
    }

    g_panes[0] = build_calendar_pane(body);
    g_panes[1] = build_stopwatch_pane(body);
    g_panes[2] = build_countdown_pane(body);

    refresh_today_from_rtc();
    apply_tab_ui();
    cal_refresh_view();
    sw_refresh_view();
    cd_refresh_view();
    return scr;
}

void ui_screen_clock_tools_bg_tick(void)
{
    if (g_cd_running && cd_get_remain_ms() == 0U)
    {
        g_cd_running   = false;
        g_cd_finished  = true;
        g_cd_remain_ms = 0U;
        xz_play_wake_sound(); /* Pleasant ding chime on countdown expiry */
    }
}

bool ui_screen_clock_tools_is_running(void)
{
    return g_sw_running || g_cd_running;
}

void ui_screen_clock_tools_update(void)
{
    ui_update_chrome(&g_chrome);
    int prev_year = g_today_year, prev_month = g_today_month, prev_day = g_today_day;
    bool had_rtc = g_cal_inited_from_rtc;
    refresh_today_from_rtc();
    if (g_active_tab == 0)
    {
        bool sd_changed = g_cal_sd_mounted != !!sdcard_is_mounted() || g_cal_sd_gen != sdcard_get_mount_gen();
        if (sd_changed) cal_reload_anniversaries();
        if (sd_changed || had_rtc != g_cal_inited_from_rtc ||
            prev_year != g_today_year || prev_month != g_today_month || prev_day != g_today_day)
            cal_refresh_view();
    }
    else if (g_active_tab == 1) sw_refresh_view();
    else                        cd_refresh_view();
}
