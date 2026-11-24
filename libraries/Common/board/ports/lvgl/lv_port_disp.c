/*******************************************************************************
#include <packages/lvgl_9.2.0/src/draw/sw/lv_draw_sw.h>
* File Name        : lv_port_disp.c
*
* Description      : This file provides implementation of low level display
*                    device driver for LVGL.
*
* Related Document : See README.md
*
******************************************************************************/

/*******************************************************************************
* Header Files
*******************************************************************************/
#include "lv_port_disp.h"
#include <rtthread.h>
#include <stdbool.h>
#include <string.h>
#include "cy_graphics.h"

/* Global: true=LVGL UI owns display scanout, false=direct UVC CLI bypass */
volatile bool g_lvgl_active = true;
static volatile uint32_t lvgl_present_count;

uint32_t lv_port_disp_get_present_count(void)
{
    return lvgl_present_count;
}


/*******************************************************************************
* Global Variables
*******************************************************************************/
/* Two partial GPU buffers: 2 x 120 lines x 480px x 2bpp = 230,400 B total in gfx_mem. */
#define LV_GPU_BUF_LINES  120U
CY_SECTION(".cy_gpu_buf") CY_ALIGN(64) uint8_t disp_buf1[MY_DISP_HOR_RES *
                                               LV_GPU_BUF_LINES * 2U];
CY_SECTION(".cy_gpu_buf") CY_ALIGN(64) uint8_t disp_buf2[MY_DISP_HOR_RES *
                                               LV_GPU_BUF_LINES * 2U];
void *frame_buffer1 = &disp_buf1;
void *frame_buffer2 = &disp_buf2;

cy_stc_gfx_context_t gfx_context;

extern void lcd_flush_rgb565_area(const void *pixels, uint32_t x, uint32_t y,
                                  uint32_t width, uint32_t height, uint32_t src_stride,
                                  rt_bool_t present);


/*******************************************************************************
* Function Name: disp_flush
********************************************************************************
* Summary:
*  Flush the content of the internal buffer the specific area on the display.
*  You can use DMA or any hardware acceleration to do this operation in the
*  background but 'lv_disp_flush_ready()' has to be called when finished.
*
* Parameters:
*  *disp_drv: Pointer to the display driver structure to be registered by HAL.
*  *area: Pointer to the area of the screen.
*  *color_p: Pointer to the frame buffer address.
*
* Return:
*  void
*
*******************************************************************************/
static void LV_ATTRIBUTE_FAST_MEM disp_flush(lv_display_t *disp_drv, const lv_area_t *area,
        uint8_t *color_p)
{
    if (g_lvgl_active && (area != NULL)) {
        uint32_t x = (uint32_t)area->x1;
        uint32_t y = (uint32_t)area->y1;
        uint32_t width = (uint32_t)(area->x2 - area->x1 + 1);
        uint32_t height = (uint32_t)(area->y2 - area->y1 + 1);
        uint32_t src_stride = lv_draw_buf_width_to_stride(width, LV_COLOR_FORMAT_RGB565) / sizeof(uint16_t);

        bool last = lv_display_flush_is_last(disp_drv);
        lcd_flush_rgb565_area(color_p, x, y, width, height, src_stride, last);
        if (last)
            lvgl_present_count++;
    }

    lv_display_flush_ready(disp_drv);
}

void lv_port_disp_set_active(bool active)
{
    g_lvgl_active = active;
    if (active) {
        lv_obj_t *scr = lv_screen_active();
        if (scr != NULL) {
            lv_obj_invalidate(scr);
        }
    }
}


/*******************************************************************************
* Function Name: lv_port_disp_init
********************************************************************************
* Summary:
*  Initialization function for display devices supported by LittelvGL.
*   LVGL requires a buffer where it internally draws the widgets.
*   Later this buffer will passed to your display driver's `flush_cb` to copy
*   its content to your display.
*   The buffer has to be greater than 1 display row
*
*   There are 3 buffering configurations:
*   1. Create ONE buffer:
*      LVGL will draw the display's content here and writes it to your display
*
*   2. Create TWO buffer:
*      LVGL will draw the display's content to a buffer and writes it your
*      display.
*      You should use DMA to write the buffer's content to the display.
*      It will enable LVGL to draw the next part of the screen to the other
*      buffer while the data is being sent form the first buffer.
*      It makes rendering and flushing parallel.
*
*   3. Double buffering
*      Set 2 screens sized buffers and set disp_drv.full_refresh = 1.
*      This way LVGL will always provide the whole rendered screen in `flush_cb`
*      and you only need to change the frame buffer's address.
*
*
* Parameters:
*  void
*
* Return:
*  void
*
*******************************************************************************/
void lv_port_disp_init(void)
{
    memset(disp_buf1, 0, sizeof(disp_buf1));
    memset(disp_buf2, 0, sizeof(disp_buf2));

    lv_display_t *disp = lv_display_create(MY_DISP_HOR_RES, MY_DISP_VER_RES);

    lv_display_set_flush_cb(disp, disp_flush);

    lv_tick_set_cb(&rt_tick_get_millisecond);

    lv_display_set_buffers(disp, disp_buf1, disp_buf2, sizeof(disp_buf1),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    // lv_display_set_rotation(disp, LV_DISPLAY_ROTATION_270);
}



/* [] END OF FILE */
