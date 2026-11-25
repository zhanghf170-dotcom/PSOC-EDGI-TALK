/*
 * Edgi_Talk_M55_WIFISafety — Dual-Core LVGL Cockpit & Driver Fatigue Monitor
 *
 * CM55 @ 400MHz + Ethos-U55 NPU + GFXSS VG-Lite + LVGL 9.2 + Wi-Fi MJPEG/Relay
 * KEY P8.3 toggles Fatigue Monitor screen & UVC AI camera.
 */

#include <rtthread.h>
#include <rtdevice.h>
#include <board.h>
#include <dirent.h>
#include <sys/stat.h>
#include <finsh.h>
#include <msh.h>
#include <stdlib.h>
#include <malloc.h>
#include <errno.h>

#include "usbh_core.h"
#include "alert_manager.h"
#include "dualcore_bridge.h"
#include "wifi_stream.h"
#include "watch_info.h"
#include "xiaozhi.h"
#include "ui_manager.h"
#include "aht10.h"

extern int lvgl_thread_init(void);

int posix_memalign(void **memptr, size_t alignment, size_t size)
{
    void *ptr = memalign(alignment, size);
    if (ptr == RT_NULL) return ENOMEM;
    *memptr = ptr;
    return 0;
}

#define LED_PIN_G       GET_PIN(16, 6)
#define KEY_PIN         GET_PIN(8, 3)

static volatile int g_uvc_toggle_req = 0;

/* AHT20 sensor: global temp/humidity for UI, HTTP API & alert log */
float g_aht_temp = 0.0f;
float g_aht_humi = 0.0f;
static aht10_device_t aht10_dev = RT_NULL;

static void aht20_read_thread(void *parameter)
{
    (void)parameter;
    while (1) {
        if (aht10_dev != RT_NULL) {
            g_aht_temp = aht10_read_temperature(aht10_dev);
            g_aht_humi = aht10_read_humidity(aht10_dev);
        }
        rt_thread_mdelay(2000);
    }
}

static void key_uvc_isr(void *args)
{
    static rt_tick_t last_tick = 0U;
    rt_tick_t now = rt_tick_get();
    (void)args;

    if ((now - last_tick) >= rt_tick_from_millisecond(200))
    {
        last_tick = now;
        g_uvc_toggle_req = 1;
    }
}

#ifdef RT_USING_DFS
static int cmd_sdlist(int argc, char **argv)
{
    const char *path = (argc > 1) ? argv[1] : "/sdcard";
    DIR *dir; struct dirent *entry; struct stat st;
    char full[256]; int files = 0; uint32_t total = 0;

    dir = opendir(path);
    if (dir == RT_NULL) {
        rt_kprintf("sdlist: '%s' not mounted or not exist\n", path);
        return -1;
    }
    rt_kprintf("\n  %s\n  %-8s %-10s %s\n  -------- ---------- --------------------------------\n",
               path, "Size", "Type", "Name");
    while ((entry = readdir(dir)) != RT_NULL) {
        rt_snprintf(full, sizeof(full), "%s/%s", path, entry->d_name);
        if (stat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) continue;
        char sz[16];
        if (st.st_size < 1024) rt_snprintf(sz, sizeof(sz), "%uB", (unsigned)st.st_size);
        else if (st.st_size < 1048576) rt_snprintf(sz, sizeof(sz), "%.1fK", (double)(st.st_size / 1024.0f));
        else rt_snprintf(sz, sizeof(sz), "%.1fM", (double)(st.st_size / 1048576.0f));
        rt_kprintf("  %-8s %-10s %s\n", sz, "FILE", entry->d_name);
        files++; total += (uint32_t)st.st_size;
    }
    closedir(dir);
    rt_kprintf("  -------- ---------- --------------------------------\n");
    rt_kprintf("  %d file(s), %u bytes total\n\n", files, (unsigned)total);
    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_sdlist, sdlist, sdlist [path] - list SD card files);
#endif

int main(void)
{
    rt_kprintf("Hello RT-Thread\n");
    rt_kprintf("Driver Safety Cockpit - CM55 (UART2)\n");

    usbh_initialize(0, USBHS_BASE, NULL);
    alert_manager_init();
    dualcore_bridge_init();
    wifi_stream_init();
    watch_info_init();
    xiaozhi_init();

    /* AHT20 sensor on i2c1 (SCL=P9.3, SDA=P9.2) */
    aht10_dev = aht10_init(PKG_AHT10_I2C_BUS_NAME);
    if (aht10_dev == RT_NULL) {
        rt_kprintf("AHT20 sensor init failed\n");
    } else {
        rt_thread_t t = rt_thread_create("aht20", aht20_read_thread, RT_NULL,
                                         1536, 15, 20);
        if (t) rt_thread_startup(t);
        rt_kprintf("AHT20 sensor OK\n");
    }

    /* Start LVGL 9.2 + VG-Lite cockpit UI thread */
    lvgl_thread_init();

    rt_pin_mode(LED_PIN_G, PIN_MODE_OUTPUT);
    rt_pin_mode(KEY_PIN, PIN_MODE_INPUT_PULLUP);
    rt_pin_attach_irq(KEY_PIN, PIN_IRQ_MODE_FALLING, key_uvc_isr, RT_NULL);
    rt_pin_irq_enable(KEY_PIN, PIN_IRQ_ENABLE);

    rt_kprintf("  KEY(P8.3)=Fatigue/Gemini Voice | xz_status / xz_talk / wifi_save / ipc_ping\n");

    int led_state = PIN_LOW;
    int tick_cnt = 0;
    while (1) {
        if (g_uvc_toggle_req) {
            g_uvc_toggle_req = 0;
            if (ui_get_active_screen() == UI_SCR_XIAOZHI) {
                rt_kprintf("[KEY] Gemini-chan XiaoZhi Voice Trigger\n");
                xiaozhi_trigger_talk();
            } else {
                rt_kprintf("[KEY] Toggle Fatigue Monitor UI\n");
                ui_request_toggle_fatigue();
            }
        }

        if (++tick_cnt >= 10) {
            tick_cnt = 0;
            led_state = (led_state == PIN_LOW) ? PIN_HIGH : PIN_LOW;
            rt_pin_write(LED_PIN_G, led_state);
        }
        rt_thread_mdelay(50);
    }

    return 0;
}
