#include <rtthread.h>

#ifdef RT_USING_DFS
#include <dfs_fs.h>
#ifdef BSP_USING_FLASH
#include <fal.h>
#endif
#include <drivers/mmcsd_core.h>
#include "dfs_romfs.h"
#include <stddef.h>

#define DBG_TAG "app.filesystem"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

static volatile rt_bool_t g_sd_mounted = RT_FALSE;
static volatile uint32_t  g_sd_mount_gen = 0U;
static volatile rt_bool_t g_sd_remount_req = RT_FALSE;

#ifdef BSP_USING_SDIO1
extern rt_bool_t   sdio1_is_card_present(void);
extern rt_uint32_t sdio1_get_err_streak(void);
extern rt_uint32_t sdio1_get_total_errors(void);
extern rt_uint32_t sdio1_get_clock(void);
extern void        sdio1_trigger_redetect(rt_bool_t probe_after_remove);
#else
static rt_bool_t   sdio1_is_card_present(void) { return g_sd_mounted; }
static rt_uint32_t sdio1_get_err_streak(void) { return 0U; }
static rt_uint32_t sdio1_get_total_errors(void) { return 0U; }
static rt_uint32_t sdio1_get_clock(void) { return 0U; }
static void        sdio1_trigger_redetect(rt_bool_t probe_after_remove) { (void)probe_after_remove; }
#endif

rt_bool_t sdcard_is_mounted(void)
{
    return g_sd_mounted;
}

uint32_t sdcard_get_mount_gen(void)
{
    return g_sd_mount_gen;
}

rt_bool_t sdcard_is_present(void)
{
    return sdio1_is_card_present();
}

void sdcard_request_remount(void)
{
    g_sd_remount_req = RT_TRUE;
}

rt_bool_t sdcard_wait_mounted(rt_int32_t timeout_ms)
{
    rt_tick_t start = rt_tick_get();
    rt_tick_t limit = (timeout_ms > 0) ? rt_tick_from_millisecond((rt_uint32_t)timeout_ms) : 0U;

    while (!g_sd_mounted)
    {
        if (timeout_ms <= 0 || (rt_tick_get() - start) >= limit)
        {
            break;
        }
        rt_thread_mdelay(50);
    }
    return g_sd_mounted;
}

#ifndef BSP_USING_XiaoZhi
static struct romfs_dirent _romfs_root[] =
{
#ifdef BSP_USING_FLASH
    {ROMFS_DIRENT_DIR, "flash", RT_NULL, 0},
#endif
#ifdef BSP_USING_SDCARD
    {ROMFS_DIRENT_DIR, "sdcard", RT_NULL, 0},
#endif
};

const struct romfs_dirent romfs_root =
{
    ROMFS_DIRENT_DIR, "/", (rt_uint8_t *)_romfs_root, sizeof(_romfs_root) / sizeof(_romfs_root[0])
};
#endif

static const char *find_sd_block_device(void)
{
    static const char *const sd_device_names[] = {"sd", "sd0", "sd1", "sd2"};
    for (size_t i = 0; i < sizeof(sd_device_names) / sizeof(sd_device_names[0]); i++)
    {
        if (rt_device_find(sd_device_names[i]) != RT_NULL)
        {
            return sd_device_names[i];
        }
    }
    return RT_NULL;
}

static rt_bool_t try_mount_sd_device(const char *device_name)
{
    if (device_name == RT_NULL)
    {
        return RT_FALSE;
    }

    rt_thread_mdelay(150);
    if (dfs_mount(device_name, "/sdcard", "elm", 0, 0) == RT_EOK)
    {
        g_sd_mount_gen++;
        g_sd_mounted = RT_TRUE;
        LOG_I("sd card '%s' mounted to '/sdcard' (gen=%lu)", device_name, (unsigned long)g_sd_mount_gen);
        return RT_TRUE;
    }

    LOG_W("Unable to mount SD card '%s' as FAT", device_name);
    return RT_FALSE;
}

static void _sdcard_mount(void)
{
#ifdef BSP_USING_SDCARD
    const char *device_name = find_sd_block_device();
    rt_tick_t last_probe_tick = 0;

    if (device_name != RT_NULL)
    {
        (void)try_mount_sd_device(device_name);
    }

    /* Clear stale CD notification and keep hot-plug & I/O recovery monitor running */
    mmcsd_wait_cd_changed(0);
    if (!g_sd_mounted)
    {
        LOG_I("SD card not present yet; hot-plug monitor active");
    }

    while (1)
    {
        rt_bool_t present = sdcard_is_present();
        rt_bool_t force_remount = g_sd_remount_req;

        if (g_sd_mounted)
        {
            if (!present)
            {
                g_sd_mounted = RT_FALSE;
                dfs_unmount("/sdcard");
                sdio1_trigger_redetect(RT_FALSE);
                g_sd_remount_req = RT_FALSE;
                LOG_W("SD card removed (CD high), unmounted '/sdcard'");
            }
            else if (find_sd_block_device() == RT_NULL)
            {
                g_sd_mounted = RT_FALSE;
                dfs_unmount("/sdcard");
                LOG_W("SD block device disappeared, unmounted '/sdcard'");
            }
            else if (force_remount || sdio1_get_err_streak() >= 3U)
            {
                LOG_W("SD card I/O recovery triggered (force=%d err_streak=%lu)",
                      force_remount ? 1 : 0, (unsigned long)sdio1_get_err_streak());
                g_sd_remount_req = RT_FALSE;
                g_sd_mounted = RT_FALSE;
                dfs_unmount("/sdcard");
                sdio1_trigger_redetect(RT_TRUE);
                last_probe_tick = rt_tick_get();
                continue;
            }
        }
        else
        {
            if (present)
            {
                const char *dev = find_sd_block_device();
                if (dev != RT_NULL && !force_remount)
                {
                    if (try_mount_sd_device(dev))
                    {
                        g_sd_remount_req = RT_FALSE;
                        continue;
                    }
                }
                if (force_remount || (rt_tick_get() - last_probe_tick) >= rt_tick_from_millisecond(2500))
                {
                    g_sd_remount_req = RT_FALSE;
                    last_probe_tick = rt_tick_get();
                    sdio1_trigger_redetect(RT_TRUE);
                    dev = find_sd_block_device();
                    if (dev != RT_NULL)
                    {
                        (void)try_mount_sd_device(dev);
                    }
                }
            }
            else
            {
                g_sd_remount_req = RT_FALSE;
                if (find_sd_block_device() != RT_NULL)
                {
                    sdio1_trigger_redetect(RT_FALSE);
                }
            }
        }

        (void)mmcsd_wait_cd_changed(rt_tick_from_millisecond(500));
    }
#endif /* BSP_USING_SDCARD */
}

#ifdef BSP_USING_FLASH
static void _fal_mount(void)
{
    struct rt_device *flash_dev = fal_mtd_nor_device_create("filesystem");
    if (flash_dev == NULL)
    {
        LOG_E("Can't create block device for filesystem");
        return;
    }
    else
    {
        LOG_I("Block device created for filesystem");

        rt_thread_mdelay(200);

        /* Try to mount filesystem */
        if (dfs_mount("filesystem", "/flash", "lfs", 0, 0) != 0)
        {
            LOG_E("Mount filesystem failed, try to mkfs");

            /* Format filesystem */
            rt_thread_mdelay(200);

            if (dfs_mkfs("lfs", "filesystem") != 0)
            {
                LOG_E("mkfs failed");
                return;
            }
            else
            {
                LOG_I("Filesystem formatted");

                /* Mount after format */
                if (dfs_mount("filesystem", "/flash", "lfs", 0, 0) != 0)
                {
                    LOG_E("Mount filesystem failed after mkfs");
                    return;
                }
                else
                {
                    LOG_I("Filesystem mounted successfully");
                }
            }
        }
        else
        {
            LOG_I("Filesystem mounted successfully");
        }
    }
}
#endif /* BSP_USING_FLASH */

static void sd_mount_thread(void *parameter)
{
    (void)parameter;
    rt_thread_mdelay(150);
    _sdcard_mount();
}

int mnt_init(void)
{
    if (dfs_mount(RT_NULL, "/", "rom", 0, &(romfs_root)) != 0)
    {
        LOG_E("rom mount to '/' failed!");
        return -RT_ERROR;
    }

#ifdef BSP_USING_FLASH
    fal_init();
    /* Mount FAL filesystem to /flash */
    _fal_mount();
#endif

#ifdef BSP_USING_SDCARD
    rt_thread_t tid;

    /* Try SD card mount & hot-plug watch in a dedicated thread */
    tid = rt_thread_create("sd_mount", sd_mount_thread, RT_NULL,
                           3072, 12, 20);
    if (tid != RT_NULL)
    {
        rt_thread_startup(tid);
    }
    else
    {
        LOG_E("create sd_mount thread err!");
    }
#endif

    return RT_EOK;
}
INIT_ENV_EXPORT(mnt_init);

static int sd_status(void)
{
    rt_kprintf("SD mounted=%d gen=%lu cd=%d err_streak=%lu total_err=%lu clk=%u\n",
               g_sd_mounted ? 1 : 0, (unsigned long)g_sd_mount_gen,
               sdcard_is_present() ? 1 : 0,
               (unsigned long)sdio1_get_err_streak(),
               (unsigned long)sdio1_get_total_errors(),
               (unsigned int)sdio1_get_clock());
    return RT_EOK;
}
MSH_CMD_EXPORT(sd_status, Show SD card mount status);

static int sd_reinit(void)
{
    uint32_t prev_gen = g_sd_mount_gen;
    sdcard_request_remount();
    for (int i = 0; i < 70; i++)
    {
        rt_thread_mdelay(50);
        if (!g_sd_remount_req && (g_sd_mounted || g_sd_mount_gen != prev_gen || !sdcard_is_present()))
        {
            break;
        }
    }
    return sd_status();
}
MSH_CMD_EXPORT(sd_reinit, Force SD card re-probe and remount);

#endif
