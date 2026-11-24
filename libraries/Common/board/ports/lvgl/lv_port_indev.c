/*******************************************************************************
* File Name        : lv_port_indev.c
*
* Description      : This file provides implementation of low level input device
*                    driver for LVGL.
*
* Related Document : See README.md
*
******************************************************************************/

/*******************************************************************************
* Header Files
*******************************************************************************/
#include "lv_port_indev.h"
#include "lv_port_disp.h"
#include "cy_utils.h"
#include "drv_touch.h"
#include "cybsp.h"

/*******************************************************************************
* Global Variables
*******************************************************************************/
lv_indev_t *indev_touchpad;
static bool g_touch_ready = false;

static int32_t touch_clamp(int32_t value, int32_t min, int32_t max)
{
    if (value < min)
    {
        return min;
    }
    if (value > max)
    {
        return max;
    }
    return value;
}

static void touchpad_init(void)
{
    if (rt_hw_ST7102_port() == RT_EOK)
    {
        g_touch_ready = true;
    }
    else
    {
        g_touch_ready = false;
        rt_kprintf("[touch] ST7102 init failed, running without touch\n");
    }
}

static void touchpad_read(lv_indev_t *indev_drv, lv_indev_data_t *data)
{
    static rt_int16_t last_x = 0;
    static rt_int16_t last_y = 0;
    rt_int16_t touch_x = 0;
    rt_int16_t touch_y = 0;

    (void)indev_drv;
    data->state = LV_INDEV_STATE_REL;

    if (g_touch_ready && (ST7102_get_single_touch(&touch_x, &touch_y) == RT_EOK))
    {
        last_x = (rt_int16_t)touch_clamp((int32_t)touch_x, 0, (int32_t)MY_DISP_HOR_RES - 1);
        last_y = (rt_int16_t)touch_clamp((int32_t)touch_y, 0, (int32_t)MY_DISP_VER_RES - 1);
        data->state = LV_INDEV_STATE_PR;
    }

    data->point.x = last_x;
    data->point.y = last_y;
}

void lv_port_indev_init(void)
{
    /* Initialize your touchpad if you have. */
    touchpad_init();

    /* Register a touchpad input device */
    indev_touchpad = lv_indev_create();
    lv_indev_set_type(indev_touchpad, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev_touchpad, touchpad_read);
}


/* [] END OF FILE */
