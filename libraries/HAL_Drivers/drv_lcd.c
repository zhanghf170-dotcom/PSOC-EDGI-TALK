/*
 * Copyright (c) 2006-2018, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2025-01-06     yuanjie      first version
 */

#include <rtthread.h>
#include <board.h>

#ifdef BSP_USING_LCD

#include <rtdevice.h>
#include "cy_graphics.h"
#include "cy_axidmac.h"
#include "cycfg_pins.h"
#include <display_tl043wvv02.h>
#include <board.h>
#include "vg_lite.h"
#include "vg_lite_platform.h"
#ifdef PKG_USING_CPU_USAGE
    #include <cpu_usage.h>
#endif
#ifdef BSP_USING_LVGL
    #include "lv_port_disp.h"
#endif

#define DRV_DEBUG
#define LOG_TAG             "drv.lcd"
#include <drv_log.h>

/* 10.1" display pin configuration */
#define DISPLAY_RESET_PORT                  GPIO_PRT20
#define DISPLAY_RESET_PIN                   (7U)

#define LCD_WIDTH           480
#define LCD_HEIGHT          800
#define LCD_STRIDE          512
#define LCD_PIXEL_CLOCK_KHZ 38986U  /* 655 pixels x 992 lines x 60 Hz */
static volatile uint32_t lcd_scanout_count;

uint32_t lcd_get_scanout_count(void)
{
    return lcd_scanout_count;
}
#define LCD_BITS_PER_PIXEL  16
#define LCD_BUF_SIZE        (LCD_STRIDE * LCD_HEIGHT * LCD_BITS_PER_PIXEL / 8)
#define LCD_PIXEL_FORMAT    RTGRAPHIC_PIXEL_FORMAT_RGB565
#define LCD_DEVICE(dev)     (struct drv_lcd_device*)(dev)
#define LCD_BL_PWM_DEV_NAME      "pwm18"
#define LCD_BL_PWM_CHANNEL       0
#define LCD_BL_PWM_PERIOD_NS     200000
#define LCD_BL_DEFAULT_BRIGHTNESS 80
#define LCD_AXIDMAC_CHANNEL      0U
#define LCD_AXIDMAC_TIMEOUT_MS   20U
#define LCD_AXIDMAC_AREA_COPY_MIN_BYTES (8U * 1024U)
#define LCD_AXIDMAC_ERROR_MASK   (CY_AXIDMAC_INTR_SRC_BUS_ERROR | \
                                  CY_AXIDMAC_INTR_DST_BUS_ERROR | \
                                  CY_AXIDMAC_INTR_INVALID_DESCR_TYPE | \
                                  CY_AXIDMAC_INTR_CURR_PTR_NULL | \
                                  CY_AXIDMAC_INTR_ACTIVE_CH_DISABLED | \
                                  CY_AXIDMAC_INTR_DESCR_BUS_ERROR)
#define GPU_TESSELLATION_BUFFER_SIZE        ((LCD_WIDTH) * 128U)
#define APP_BUFFER_COUNT                    (2U)
#define DEFAULT_GPU_CMD_BUFFER_SIZE         ((64U) * (512))
#define VG_PARAMS_POS                       (0UL)
#define GPU_MEM_BASE                        (0x0U)
#define VGLITE_HEAP_EXTRA_SIZE  (64U * 1024U)
#define VGLITE_HEAP_SIZE        (((DEFAULT_GPU_CMD_BUFFER_SIZE) * \
                                 (APP_BUFFER_COUNT)) + \
                                 ((GPU_TESSELLATION_BUFFER_SIZE) * \
                                 (APP_BUFFER_COUNT)) + \
                                 (VGLITE_HEAP_EXTRA_SIZE))
CY_SECTION(".cy_gpu_buf") CY_ALIGN(64) uint8_t contiguous_mem[VGLITE_HEAP_SIZE] = { 0xFF };
volatile void *vglite_heap_base = &contiguous_mem;

struct drv_lcd_device _lcd;

GFXSS_Type *gfxbase = (GFXSS_Type*) GFXSS;
static cy_stc_gfx_context_t lcd_gfx_context;

CY_SECTION(".cy_gpu_buf") CY_ALIGN(64) uint8_t graphics_buffer[LCD_BUF_SIZE] = {0xFF};
CY_SECTION(".cy_socmem_data") CY_ALIGN(8) static cy_stc_axidmac_descriptor_t lcd_axidmac_descriptor;
static rt_mutex_t lcd_axidmac_lock;
static rt_uint8_t g_lcd_brightness = LCD_BL_DEFAULT_BRIGHTNESS;

struct drv_lcd_device
{
    struct rt_device parent;

    struct rt_device_graphic_info lcd_info;

    struct rt_semaphore lcd_lock;

    /* 0:front_buf is being used 1: back_buf is being used*/
    rt_uint8_t cur_buf;
    rt_uint8_t *front_buf;
    rt_uint8_t *back_buf;
};
#ifdef RT_USING_PWM
static struct rt_device_pwm *g_lcd_bl_pwm = RT_NULL;
static rt_err_t lcd_backlight_set(rt_uint8_t percent)
{
    struct rt_device_pwm *pwm = g_lcd_bl_pwm;
    rt_err_t result;

    if (percent > 100U)
    {
        percent = 100U;
    }

    if (pwm == RT_NULL)
    {
        pwm = (struct rt_device_pwm *)rt_device_find(LCD_BL_PWM_DEV_NAME);
        if (pwm == RT_NULL)
        {
            LOG_W("Cannot find %s device", LCD_BL_PWM_DEV_NAME);
            return -RT_ENOSYS;
        }
    }

    rt_uint32_t pulse = (LCD_BL_PWM_PERIOD_NS * percent) / 100U;
    result = rt_pwm_set(pwm, LCD_BL_PWM_CHANNEL, LCD_BL_PWM_PERIOD_NS, pulse);
    if (result != RT_EOK)
        return result;

    if (g_lcd_bl_pwm == RT_NULL)
    {
        result = rt_pwm_enable(pwm, LCD_BL_PWM_CHANNEL);
        if (result != RT_EOK)
            return result;

        /* Board power-up holds the backlight pin high until PWM is running. */
        Cy_GPIO_SetHSIOM(CYBSP_DISP_BACKLIGHT_PWM_PORT, CYBSP_DISP_BACKLIGHT_PWM_PIN,
                         CYBSP_DISP_BACKLIGHT_PWM_HSIOM);
        g_lcd_bl_pwm = pwm;
    }

    g_lcd_brightness = percent;
    return RT_EOK;
}
#endif

rt_err_t lcd_set_brightness(rt_uint8_t percent)
{
#ifdef RT_USING_PWM
    return lcd_backlight_set(percent);
#else
    (void)percent;
    return -RT_ENOSYS;
#endif
}

rt_uint8_t lcd_get_brightness(void)
{
    return g_lcd_brightness;
}

static rt_bool_t lcd_addr_is_cacheable(const void *addr)
{
    uintptr_t a = (uintptr_t)addr;

    if ((a >= 0x20000000UL && a <= 0x2003FFFFUL) ||
        (a >= 0x26370000UL && a <= 0x264FFFFFUL) ||
        (a >= 0x64400000UL && a <= 0x64FFFFFFUL))
    {
        return RT_FALSE;
    }

    return RT_TRUE;
}

static void lcd_dcache_clean_range(const void *addr, uint32_t size)
{
#if defined(__DCACHE_PRESENT) && (__DCACHE_PRESENT == 1U)
    if ((size == 0U) || ((SCB->CCR & SCB_CCR_DC_Msk) == 0U) || !lcd_addr_is_cacheable(addr))
    {
        return;
    }
    uintptr_t start = (uintptr_t)addr & ~((uintptr_t)__SCB_DCACHE_LINE_SIZE - 1U);
    uintptr_t end = ((uintptr_t)addr + size + __SCB_DCACHE_LINE_SIZE - 1U) &
                    ~((uintptr_t)__SCB_DCACHE_LINE_SIZE - 1U);
    SCB_CleanDCache_by_Addr((void *)start, (int32_t)(end - start));
#else
    (void)addr;
    (void)size;
#endif
}

static rt_bool_t lcd_axidmac_copy_rgb565_area(const uint16_t *src_pixels,
                                              uint32_t src_stride,
                                              uint32_t dst_x,
                                              uint32_t dst_y,
                                              uint32_t width,
                                              uint32_t height)
{
    const uint32_t row_bytes = width * sizeof(uint16_t);
    const uint32_t total_bytes = row_bytes * height;
    const int32_t src_x_incr = (int32_t)(src_stride * sizeof(uint16_t));
    const int32_t dst_x_incr = (int32_t)(LCD_STRIDE * sizeof(uint16_t));
    uint16_t *dst_pixels = ((uint16_t *)graphics_buffer) + (dst_y * LCD_STRIDE) + dst_x;
    rt_tick_t deadline;
    uint32_t intr;

    if ((src_pixels == RT_NULL) || (width == 0U) || (height == 0U))
    {
        return RT_FALSE;
    }

    if ((total_bytes < LCD_AXIDMAC_AREA_COPY_MIN_BYTES) ||
        (row_bytes > CY_AXIDMAC_LOOP_COUNT_MAX) ||
        (height > CY_AXIDMAC_LOOP_COUNT_MAX) ||
        (src_x_incr > CY_AXIDMAC_LOOP_INCREMENT_MAX) ||
        (dst_x_incr > CY_AXIDMAC_LOOP_INCREMENT_MAX))
    {
        return RT_FALSE;
    }

    if ((lcd_axidmac_lock != RT_NULL) &&
        (rt_mutex_take(lcd_axidmac_lock,
                       rt_tick_from_millisecond(LCD_AXIDMAC_TIMEOUT_MS)) != RT_EOK))
    {
        return RT_FALSE;
    }

    {
        cy_stc_axidmac_descriptor_config_t descriptor_config =
        {
            .retrigger = CY_AXIDMAC_RETRIG_IM,
            .interruptType = CY_AXIDMAC_DESCR,
            .triggerOutType = CY_AXIDMAC_DESCR,
            .channelState = CY_AXIDMAC_CHANNEL_DISABLED,
            .triggerInType = CY_AXIDMAC_DESCR,
            .dataPrefetch = true,
            .descriptorType = CY_AXIDMAC_2D_MEMORY_COPY,
            .srcAddress = (void *)src_pixels,
            .dstAddress = (void *)dst_pixels,
            .mCount = row_bytes,
            .srcXincrement = (int16_t)src_x_incr,
            .dstXincrement = (int16_t)dst_x_incr,
            .xCount = height,
            .srcYincrement = 0,
            .dstYincrement = 0,
            .yCount = 1U,
            .nextDescriptor = RT_NULL,
        };
        cy_stc_axidmac_channel_config_t channel_config =
        {
            .descriptor = &lcd_axidmac_descriptor,
            .priority = 2U,
            .enable = false,
            .bufferable = true,
        };

        Cy_AXIDMAC_Channel_Disable(SAXI_DMAC, LCD_AXIDMAC_CHANNEL);
        Cy_AXIDMAC_Channel_ClearInterrupt(SAXI_DMAC, LCD_AXIDMAC_CHANNEL, CY_AXIDMAC_INTR_MASK);

        if ((Cy_AXIDMAC_Descriptor_Init(&lcd_axidmac_descriptor, &descriptor_config) != CY_AXIDMAC_SUCCESS) ||
            (Cy_AXIDMAC_Channel_Init(SAXI_DMAC, LCD_AXIDMAC_CHANNEL, &channel_config) != CY_AXIDMAC_SUCCESS))
        {
            if (lcd_axidmac_lock != RT_NULL)
            {
                rt_mutex_release(lcd_axidmac_lock);
            }
            return RT_FALSE;
        }
    }

    lcd_dcache_clean_range(src_pixels, ((height - 1U) * src_stride * sizeof(uint16_t)) + row_bytes);
    lcd_dcache_clean_range(&lcd_axidmac_descriptor, sizeof(lcd_axidmac_descriptor));

    Cy_AXIDMAC_Enable(SAXI_DMAC);
    Cy_AXIDMAC_Channel_Enable(SAXI_DMAC, LCD_AXIDMAC_CHANNEL);
    Cy_AXIDMAC_Channel_SetSwTrigger(SAXI_DMAC, LCD_AXIDMAC_CHANNEL);

    deadline = rt_tick_get() + rt_tick_from_millisecond(LCD_AXIDMAC_TIMEOUT_MS);
    do
    {
        intr = Cy_AXIDMAC_Channel_GetInterruptStatus(SAXI_DMAC, LCD_AXIDMAC_CHANNEL);
        if ((intr & LCD_AXIDMAC_ERROR_MASK) != 0U)
        {
            Cy_AXIDMAC_Channel_ClearInterrupt(SAXI_DMAC, LCD_AXIDMAC_CHANNEL, intr);
            Cy_AXIDMAC_Channel_Disable(SAXI_DMAC, LCD_AXIDMAC_CHANNEL);
            if (lcd_axidmac_lock != RT_NULL)
            {
                rt_mutex_release(lcd_axidmac_lock);
            }
            return RT_FALSE;
        }

        if ((intr & CY_AXIDMAC_INTR_COMPLETION) != 0U)
        {
            Cy_AXIDMAC_Channel_ClearInterrupt(SAXI_DMAC, LCD_AXIDMAC_CHANNEL, intr);
            if (lcd_axidmac_lock != RT_NULL)
            {
                rt_mutex_release(lcd_axidmac_lock);
            }
            return RT_TRUE;
        }
    } while ((rt_int32_t)(deadline - rt_tick_get()) > 0);

    Cy_AXIDMAC_Channel_Disable(SAXI_DMAC, LCD_AXIDMAC_CHANNEL);
    if (lcd_axidmac_lock != RT_NULL)
    {
        rt_mutex_release(lcd_axidmac_lock);
    }
    return RT_FALSE;
}

void lcd_flush_rgb565_area(const void *pixels, uint32_t x, uint32_t y,
                           uint32_t width, uint32_t height, uint32_t src_stride,
                           rt_bool_t present)
{
    const uint16_t *src = (const uint16_t *)pixels;
    uint16_t *dst = (uint16_t *)graphics_buffer;
    uint32_t copy_width = width;
    uint32_t copy_height = height;

    if ((src == RT_NULL) || (width == 0U) || (height == 0U) || (src_stride < width))
    {
        return;
    }

    if ((x >= LCD_WIDTH) || (y >= LCD_HEIGHT))
    {
        return;
    }

    if ((x + copy_width) > LCD_WIDTH)
    {
        copy_width = LCD_WIDTH - x;
    }

    if ((y + copy_height) > LCD_HEIGHT)
    {
        copy_height = LCD_HEIGHT - y;
    }

    if (!lcd_axidmac_copy_rgb565_area(src, src_stride, x, y, copy_width, copy_height))
    {
        for (uint32_t row = 0U; row < copy_height; row++)
        {
            uint16_t *dst_row = dst + ((y + row) * LCD_STRIDE) + x;
            const uint16_t *src_row = src + (row * src_stride);

            memcpy(dst_row, src_row, copy_width * sizeof(uint16_t));
        }
    }

    if (present)
    {
        cy_en_gfx_status_t status = Cy_GFXSS_Set_FrameBuffer(gfxbase, (uint32_t *)graphics_buffer, &lcd_gfx_context);
        if (status == CY_GFX_SUCCESS)
        {
            (void)rt_sem_take(&_lcd.lcd_lock, RT_TICK_PER_SECOND / 20);
        }
    }
}

static rt_err_t drv_lcd_init(struct rt_device *device)
{
    (void)device;
    return RT_EOK;
}

static rt_err_t drv_lcd_control(struct rt_device *device, int cmd, void *args)
{
    struct drv_lcd_device *lcd = LCD_DEVICE(device);
    volatile cy_en_gfx_status_t status;
    switch (cmd)
    {
    case RTGRAPHIC_CTRL_RECT_UPDATE:
    {
        struct rt_device_rect_info *info = (struct rt_device_rect_info *)args;
        uint32_t start_line = 0U;
        uint32_t end_line = lcd->lcd_info.height;
        rt_bool_t try_partial = RT_FALSE;

        if (info != RT_NULL)
        {
            if ((info->width == 0U) || (info->height == 0U))
            {
                return RT_EOK;
            }
            if ((info->x >= lcd->lcd_info.width) || (info->y >= lcd->lcd_info.height))
            {
                return RT_EOK;
            }
            if ((info->x + info->width) > lcd->lcd_info.width)
            {
                info->width = lcd->lcd_info.width - info->x;
            }
            if ((info->y + info->height) > lcd->lcd_info.height)
            {
                info->height = lcd->lcd_info.height - info->y;
            }

            start_line = info->y;
            end_line = info->y + info->height;
            if ((info->x == 0U) && (info->width == lcd->lcd_info.width))
            {
                try_partial = RT_TRUE;
            }
        }

        if (try_partial &&
            ((lcd_gfx_context.dc_context.display_type == GFX_DISP_TYPE_DBI_A) ||
             (lcd_gfx_context.dc_context.display_type == GFX_DISP_TYPE_DBI_B) ||
             (lcd_gfx_context.dc_context.display_type == GFX_DISP_TYPE_DBI_C) ||
             (lcd_gfx_context.dc_context.display_type == GFX_DISP_TYPE_DSI_DBI)))
        {
            status = Cy_GFXSS_TransferPartialFrame(gfxbase, start_line, end_line, &lcd_gfx_context);
            if (CY_GFX_SUCCESS != status)
            {
                LOG_E("[%s: %d] Partial frame transfer failed. Error type: %u\r\n", __func__, __LINE__, status);
                CY_ASSERT(0);
            }
            break;
        }

        /* update */
        /* Set the frame buffer with the image pointer to be displayed on LCD */
        status = Cy_GFXSS_Set_FrameBuffer(gfxbase, (uint32_t*)graphics_buffer, &lcd_gfx_context);

        /* Image rendering failed. Stop program execution */
        if (CY_GFX_SUCCESS != status)
        {
            LOG_E("[%s: %d] Image rendering failed. Error type: %u\r\n", __func__, __LINE__, status);
            CY_ASSERT(0);
        }
        rt_sem_take(&_lcd.lcd_lock, RT_TICK_PER_SECOND / 20);
    }
    break;

    case RTGRAPHIC_CTRL_GET_INFO:
    {
        struct rt_device_graphic_info *info = (struct rt_device_graphic_info *)args;

        RT_ASSERT(info != RT_NULL);
        info->pixel_format  = lcd->lcd_info.pixel_format;
        info->bits_per_pixel = 16;
        info->pitch         = lcd->lcd_info.pitch;
        info->width         = lcd->lcd_info.width;
        info->height        = lcd->lcd_info.height;
        info->framebuffer   = lcd->lcd_info.framebuffer;
        info->smem_len      = lcd->lcd_info.smem_len;
    }
    break;
    }

    return RT_EOK;
}

cy_stc_sysint_t dc_irq_cfg =
{
    .intrSrc = GFXSS_DC_IRQ,
    .intrPriority = 3U //DC_INT_PRIORITY
};
cy_stc_sysint_t gpu_irq_cfg =
{
    .intrSrc      = GFXSS_GPU_IRQ,
    .intrPriority = 3U //GPU_INT_PRIORITY
};

#ifdef PKG_USING_CPU_USAGE
uint32_t calculate_idle_percentage(void)
{

    uint32_t load;
    cpu_usage_init();
    load = cpu_load_average();

    return 100 - load;
}
#endif

static void dc_irq_handler(void)
{
    rt_interrupt_enter();
    const uint32_t events = gfxbase->GFXSS_DC.DCNANO.GCREGDISPLAYINTR;
    gfxbase->GFXSS_DC.MXDC.INTR = UINT32_MAX;
    if (events & GFXSS_DC_DCNANO_GCREGDISPLAYINTR_GCREGDISPLAYINTR_DISP0_Msk)
    {
        lcd_scanout_count++;
        rt_sem_release(&_lcd.lcd_lock);
    }
    rt_interrupt_leave();
}

static void gpu_irq_handler(void)
{
    rt_interrupt_enter();
    Cy_GFXSS_Clear_GPU_Interrupt(gfxbase, &lcd_gfx_context);
    vg_lite_IRQHandler();
    rt_interrupt_leave();
}

rt_err_t psoc_lcd_init(struct drv_lcd_device *lcd)
{
    /* 10.1" display pin configuration */
    mtb_display_tl043wvv02_pin_config_t tl043wvv02_pin_cfg =
    {
        .reset_port = DISPLAY_RESET_PORT,
        .reset_pin  = DISPLAY_RESET_PIN,
    };

    static cy_en_gfx_status_t gfx_status;
    static cy_en_sysint_status_t status;
    volatile cy_en_mipidsi_status_t mipi_status;

    /* Match the Edgi-Talk BSP panel timing while keeping aligned framebuffer rows. */
    GFXSS_graphics_layer.width = LCD_WIDTH;
    GFXSS_dc_config.display_width = LCD_WIDTH;
    GFXSS_mipidsi_display_params.hdisplay = LCD_WIDTH;
    GFXSS_mipidsi_display_params.pixel_clock = LCD_PIXEL_CLOCK_KHZ;
    GFXSS_mipidsi_display_params.hfp = 86U;
    GFXSS_mipidsi_display_params.hbp = 87U;
    GFXSS_mipidsi_display_params.vfp = 182U;
    GFXSS_mipidsi_display_params.vbp = 8U;
    GFXSS_mipi_dsi_config.per_lane_mbps = 900U;

    /* Graphics subsystem initialization failed. Stop program execution */
    gfx_status = Cy_GFXSS_Init(gfxbase, &GFXSS_config, &lcd_gfx_context);
    if (CY_GFX_SUCCESS != gfx_status)
    {
        LOG_E("[%s: %d] Gfxss initialization failed. Error type: %u\r\n", __func__, __LINE__, status);
        return -RT_ERROR;
    }

    gfxbase->GFXSS_DC.DCNANO.GCREGFRAMEBUFFERSTRIDE = LCD_STRIDE * sizeof(uint16_t);

    /* Initialize GFXXs DC interrupt */
    status = Cy_SysInt_Init(&dc_irq_cfg, dc_irq_handler);
    if (CY_SYSINT_SUCCESS != status)
    {
        LOG_E("Error in registering DC interrupt: %d\r\n", status);
        return -RT_ERROR;
    }

    /* Enable interrupt in NVIC. */
    NVIC_EnableIRQ(GFXSS_DC_IRQ);

    status = Cy_SysInt_Init(&gpu_irq_cfg, gpu_irq_handler);
    if (CY_SYSINT_SUCCESS != status)
    {
        LOG_E("Error in registering GPU interrupt: %d\r\n", status);
        CY_ASSERT(0);
    }

    /* Enable GPU interrupt */
    Cy_GFXSS_Enable_GPU_Interrupt(GFXSS);

    /* Enable GFX GPU interrupt in NVIC. */
    NVIC_EnableIRQ(GFXSS_GPU_IRQ);

    mipi_status = mtb_display_tl043wvv02_init(GFXSS_GFXSS_MIPIDSI, &tl043wvv02_pin_cfg);

    /* Display initialization failed. Stop program execution */
    if (CY_MIPIDSI_SUCCESS != mipi_status)
    {
        LOG_E("[%s: %d] Display initialization failed. Error type: %u\r\n", __func__, __LINE__, status);
        return -RT_ERROR;
    }
    /* Display-on commits the panel's packed pitch. Commit the DMA pitch with our buffer. */
    gfxbase->GFXSS_DC.DCNANO.GCREGFRAMEBUFFERSTRIDE = LCD_STRIDE * sizeof(uint16_t);
    gfx_status = Cy_GFXSS_Set_FrameBuffer(gfxbase, (uint32_t *)graphics_buffer, &lcd_gfx_context);
    if (gfx_status != CY_GFX_SUCCESS)
    {
        LOG_E("Initial framebuffer bind failed: %u", (unsigned)gfx_status);
        return -RT_ERROR;
    }

    LOG_I("init screen success (%ux%u, %u-byte pitch)",
          (unsigned)LCD_WIDTH, (unsigned)LCD_HEIGHT,
          (unsigned)gfxbase->GFXSS_DC.DCNANO.GCREGFRAMEBUFFERSTRIDE);
    return RT_EOK;
}

#ifdef RT_USING_DEVICE_OPS
const static struct rt_device_ops lcd_ops =
{
    drv_lcd_init,
    RT_NULL,
    RT_NULL,
    RT_NULL,
    RT_NULL,
    drv_lcd_control
};
#endif

int drv_lcd_hw_init(void)
{
    rt_err_t result = RT_EOK;
    struct rt_device *device = &_lcd.parent;

    /* memset _lcd to zero */
    memset(&_lcd, 0x00, sizeof(_lcd));

    /* init lcd_lock semaphore */
    result = rt_sem_init(&_lcd.lcd_lock, "lcd_lock", 0, RT_IPC_FLAG_FIFO);
    if (result != RT_EOK)
    {
        LOG_E("init semaphore failed!\n");
        result = -RT_ENOMEM;
        goto __exit;
    }

    lcd_axidmac_lock = rt_mutex_create("lcd_dma", RT_IPC_FLAG_FIFO);
    if (lcd_axidmac_lock == RT_NULL)
    {
        LOG_E("init axidmac mutex failed!\n");
        result = -RT_ENOMEM;
        goto __exit;
    }

    /* config LCD dev info */
    _lcd.lcd_info.height = LCD_HEIGHT;
    _lcd.lcd_info.width = LCD_WIDTH;
    _lcd.lcd_info.bits_per_pixel = LCD_BITS_PER_PIXEL;
    _lcd.lcd_info.pixel_format = LCD_PIXEL_FORMAT;
    _lcd.lcd_info.pitch = LCD_STRIDE * (LCD_BITS_PER_PIXEL / 8);
    _lcd.lcd_info.smem_len = LCD_BUF_SIZE;

    /* malloc memory for Triple Buffering */
    // _lcd.front_buf=_lcd.lcd_info.framebuffer = rt_malloc_align(LCD_BUF_SIZE, 32);
    _lcd.front_buf = _lcd.lcd_info.framebuffer = graphics_buffer;


    if (_lcd.lcd_info.framebuffer == RT_NULL)
    {
        LOG_E("init frame buffer failed!\n");
        result = -RT_ENOMEM;
        goto __exit;
    }
    /* memset buff to 0xFF */
    memset(_lcd.lcd_info.framebuffer, 0x00, LCD_BUF_SIZE);
    device->type    = RT_Device_Class_Graphic;
#ifdef RT_USING_DEVICE_OPS
    device->ops     = &lcd_ops;
#else
    device->init    = drv_lcd_init;
#ifndef ART_PI_TouchGFX_LIB
    device->control = drv_lcd_control;
#endif
#endif

    /* register lcd device */
    rt_device_register(device, "lcd", RT_DEVICE_FLAG_RDWR);
    /* init mipi display */
    if (psoc_lcd_init(&_lcd) != RT_EOK)
    {
        result = -RT_ERROR;
        goto __exit;
    }
#ifdef RT_USING_PWM
    rt_err_t backlight_result = lcd_backlight_set(LCD_BL_DEFAULT_BRIGHTNESS);
    if (backlight_result != RT_EOK)
        LOG_W("Backlight PWM init failed: %d", backlight_result);
#endif
__exit:
    if (result != RT_EOK)
    {
        if (lcd_axidmac_lock != RT_NULL)
        {
            rt_mutex_delete(lcd_axidmac_lock);
            lcd_axidmac_lock = RT_NULL;
        }
        rt_sem_delete(&_lcd.lcd_lock);
    }
    if (result == RT_EOK)
    {
        _lcd.parent.control(&_lcd.parent, RTGRAPHIC_CTRL_RECT_UPDATE, RT_NULL);
    }

#ifdef BSP_USING_LVGL
    //Allocate GPU memory for LVGL
    vg_lite_error_t vglite_status = VG_LITE_SUCCESS;
    vg_module_parameters_t vg_params;
    vg_params.register_mem_base = (uint32_t)GFXSS_GFXSS_GPU_GCNANO;
    vg_params.gpu_mem_base[VG_PARAMS_POS] = GPU_MEM_BASE;
    vg_params.contiguous_mem_base[VG_PARAMS_POS] = (volatile void *) vglite_heap_base;
    vg_params.contiguous_mem_size[VG_PARAMS_POS] = VGLITE_HEAP_SIZE;
    vg_lite_init_mem(&vg_params);

    vglite_status = vg_lite_init((LCD_WIDTH) / 4,
                                 (LCD_HEIGHT) / 4);
    if (vglite_status == VG_LITE_SUCCESS)
    {
        LOG_I("Allocate GPU memory success\n");
    }
    else
    {
        LOG_E("VG-Lite init failed: %d\n", vglite_status);
    }
#endif

    return result;
}
INIT_APP_EXPORT(drv_lcd_hw_init);


#ifdef DRV_DEBUG
#ifdef FINSH_USING_MSH
int lcd_test(void)
{
    struct drv_lcd_device *lcd;
    lcd = (struct drv_lcd_device *)rt_device_find("lcd");

    //while (1)
    {
        if (lcd->lcd_info.pixel_format == RTGRAPHIC_PIXEL_FORMAT_RGB565)
        {
            /* red */
            for (int i = 0; i < LCD_BUF_SIZE / 2; i++)
            {
                lcd->lcd_info.framebuffer[2 * i] = 0x00;
                lcd->lcd_info.framebuffer[2 * i + 1] = 0xF8;
            }
            lcd->parent.control(&lcd->parent, RTGRAPHIC_CTRL_RECT_UPDATE, RT_NULL);
            rt_thread_mdelay(1000);
            /* green */
            for (int i = 0; i < LCD_BUF_SIZE / 2; i++)
            {
                lcd->lcd_info.framebuffer[2 * i] = 0xE0;
                lcd->lcd_info.framebuffer[2 * i + 1] = 0x07;
            }
            lcd->parent.control(&lcd->parent, RTGRAPHIC_CTRL_RECT_UPDATE, RT_NULL);
            rt_thread_mdelay(1000);
            /* blue */
            for (int i = 0; i < LCD_BUF_SIZE / 2; i++)
            {
                lcd->lcd_info.framebuffer[2 * i] = 0x1F;
                lcd->lcd_info.framebuffer[2 * i + 1] = 0x00;
            }
            rt_thread_mdelay(1000);
            for (int i = 0; i < LCD_BUF_SIZE / 2; i++)
            {
                lcd->lcd_info.framebuffer[2 * i] = 0xFF;
                lcd->lcd_info.framebuffer[2 * i + 1] = 0xFF;
            }
        }
        else if (lcd->lcd_info.pixel_format == RTGRAPHIC_PIXEL_FORMAT_RGB888)
        {
            /* red */
            for (int i = 0; i < LCD_BUF_SIZE / 3; i++)
            {
                lcd->lcd_info.framebuffer[3 * i] = 0x00;
                lcd->lcd_info.framebuffer[3 * i + 1] = 0x00;
                lcd->lcd_info.framebuffer[3 * i + 2] = 0xff;
            }
            lcd->parent.control(&lcd->parent, RTGRAPHIC_CTRL_RECT_UPDATE, RT_NULL);
            rt_thread_mdelay(1000);
            /* green */
            for (int i = 0; i < LCD_BUF_SIZE / 3; i++)
            {
                lcd->lcd_info.framebuffer[3 * i] = 0x00;
                lcd->lcd_info.framebuffer[3 * i + 1] = 0xff;
                lcd->lcd_info.framebuffer[3 * i + 2] = 0x00;
            }
            lcd->parent.control(&lcd->parent, RTGRAPHIC_CTRL_RECT_UPDATE, RT_NULL);
            rt_thread_mdelay(1000);
            /* blue */
            for (int i = 0; i < LCD_BUF_SIZE / 3; i++)//LCD_BUF_SIZE
            {
                lcd->lcd_info.framebuffer[3 * i] = 0xff;
                lcd->lcd_info.framebuffer[3 * i + 1] = 0x00;
                lcd->lcd_info.framebuffer[3 * i + 2] = 0x00;
            }
        }
        rt_thread_mdelay(1000);
    }
    return 0;
}
MSH_CMD_EXPORT(lcd_test, lcd_test);
#endif /* FINSH_USING_MSH */
#endif /* DRV_DEBUG */
#endif


