#include "sd_assets.h"
#include "board.h"
#include <stdio.h>

void sd_asset_dcache_clean(void *ptr, size_t len)
{
#if defined(__DCACHE_PRESENT) && (__DCACHE_PRESENT == 1U)
    if (ptr == RT_NULL || len == 0U || (SCB->CCR & SCB_CCR_DC_Msk) == 0U)
    {
        return;
    }
    uintptr_t start = (uintptr_t)ptr & ~((uintptr_t)__SCB_DCACHE_LINE_SIZE - 1U);
    uintptr_t end   = ((uintptr_t)ptr + len + (uintptr_t)__SCB_DCACHE_LINE_SIZE - 1U) &
                      ~((uintptr_t)__SCB_DCACHE_LINE_SIZE - 1U);
    SCB_CleanDCache_by_Addr((void *)start, (int32_t)(end - start));
#else
    (void)ptr;
    (void)len;
#endif
}

static int sd_asset_read_range_once(const char *path, size_t offset, void *dst, size_t len)
{
    FILE *fp;
    size_t got = 0U;
    uint8_t *out = (uint8_t *)dst;

    if (!sdcard_is_mounted())
    {
        return -RT_ENOENT;
    }

    fp = fopen(path, "rb");
    if (fp == RT_NULL)
    {
        return -RT_ENOENT;
    }

    if (offset > 0U && fseek(fp, (long)offset, SEEK_SET) != 0)
    {
        fclose(fp);
        return -RT_EIO;
    }

    while (got < len)
    {
        size_t chunk = len - got;
        if (chunk > 32768U)
        {
            chunk = 32768U;
        }
        size_t n = fread(out + got, 1U, chunk, fp);
        if (n == 0U)
        {
            break;
        }
        got += n;
    }

    fclose(fp);
    return (got == len) ? RT_EOK : -RT_EIO;
}

int sd_asset_read_range(const char *path, size_t offset, void *dst, size_t len, rt_bool_t clean_dcache)
{
    int ret = -RT_EIO;

    if (path == RT_NULL || dst == RT_NULL || len == 0U || !sdcard_is_mounted())
    {
        return -RT_EINVAL;
    }

    for (int attempt = 0; attempt < 3; attempt++)
    {
        ret = sd_asset_read_range_once(path, offset, dst, len);
        if (ret == RT_EOK || !sdcard_is_mounted())
        {
            break;
        }
        rt_thread_mdelay(20);
    }

    if (ret == RT_EOK && clean_dcache)
    {
        sd_asset_dcache_clean(dst, len);
    }

    return ret;
}

static int sd_asset_read_file_once(const char *path, void *dst, size_t max_len, size_t *out_len)
{
    FILE *fp;
    long sz;
    size_t len;
    size_t got = 0U;
    uint8_t *out = (uint8_t *)dst;

    if (!sdcard_is_mounted())
    {
        return -RT_ENOENT;
    }

    fp = fopen(path, "rb");
    if (fp == RT_NULL)
    {
        return -RT_ENOENT;
    }

    if (fseek(fp, 0L, SEEK_END) != 0)
    {
        fclose(fp);
        return -RT_EIO;
    }

    sz = ftell(fp);
    if (sz <= 0L)
    {
        fclose(fp);
        return -RT_EIO;
    }

    len = (size_t)sz;
    if (len > max_len)
    {
        fclose(fp);
        return -RT_ENOMEM;
    }

    if (fseek(fp, 0L, SEEK_SET) != 0)
    {
        fclose(fp);
        return -RT_EIO;
    }

    while (got < len)
    {
        size_t chunk = len - got;
        if (chunk > 32768U)
        {
            chunk = 32768U;
        }
        size_t n = fread(out + got, 1U, chunk, fp);
        if (n == 0U)
        {
            break;
        }
        got += n;
    }

    fclose(fp);
    if (got != len)
    {
        return -RT_EIO;
    }

    if (out_len != RT_NULL)
    {
        *out_len = len;
    }
    return RT_EOK;
}

int sd_asset_read_file(const char *path, void *dst, size_t max_len, size_t *out_len, rt_bool_t clean_dcache)
{
    int ret = -RT_EIO;
    size_t read_len = 0U;

    if (out_len != RT_NULL)
    {
        *out_len = 0U;
    }

    if (path == RT_NULL || dst == RT_NULL || max_len == 0U || !sdcard_is_mounted())
    {
        return -RT_EINVAL;
    }

    for (int attempt = 0; attempt < 3; attempt++)
    {
        read_len = 0U;
        ret = sd_asset_read_file_once(path, dst, max_len, &read_len);
        if (ret == RT_EOK || ret == -RT_ENOMEM || !sdcard_is_mounted())
        {
            break;
        }
        rt_thread_mdelay(20);
    }

    if (ret == RT_EOK)
    {
        if (out_len != RT_NULL)
        {
            *out_len = read_len;
        }
        if (clean_dcache)
        {
            sd_asset_dcache_clean(dst, read_len);
        }
    }

    return ret;
}
