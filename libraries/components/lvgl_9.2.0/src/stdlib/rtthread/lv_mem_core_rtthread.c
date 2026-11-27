/**
 * @file lv_malloc_core_rtthread.c
 */

/*********************
 *      INCLUDES
 *********************/
#include "../lv_mem.h"
#if LV_USE_STDLIB_MALLOC == LV_STDLIB_RTTHREAD
#include "../../stdlib/lv_mem.h"
#include <rtthread.h>

#if defined(COMPONENT_CM55) && defined(BSP_USING_HYPERAM) && defined(RT_USING_MEMHEAP_AS_HEAP)
extern struct rt_memheap *drv_hyperam_get_memheap(void);
#endif

#ifndef RT_USING_HEAP
    #error "lv_mem_core_rtthread: RT_USING_HEAP is required. Define it in rtconfig.h"
#endif

/*********************
 *      DEFINES
 *********************/

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 *  STATIC PROTOTYPES
 **********************/

/**********************
 *  STATIC VARIABLES
 **********************/

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

void lv_mem_init(void)
{
    return; /*Nothing to init*/
}

void lv_mem_deinit(void)
{
    return; /*Nothing to deinit*/
}

lv_mem_pool_t lv_mem_add_pool(void * mem, size_t bytes)
{
    /*Not supported*/
    LV_UNUSED(mem);
    LV_UNUSED(bytes);
    return NULL;
}

void lv_mem_remove_pool(lv_mem_pool_t pool)
{
    /*Not supported*/
    LV_UNUSED(pool);
    return;
}

void * lv_malloc_core(size_t size)
{
#if defined(COMPONENT_CM55) && defined(BSP_USING_HYPERAM) && defined(RT_USING_MEMHEAP_AS_HEAP)
    /* Keep large LVGL objects and pixels out of the on-chip runtime heap. */
    if (size >= 1024U)
    {
        struct rt_memheap *heap = drv_hyperam_get_memheap();
        if (heap && heap->pool_size != 0U)
        {
            void *buffer = rt_memheap_alloc(heap, size);
            if (buffer)
                return buffer;
        }
    }
#endif
    return rt_malloc(size);
}

void * lv_realloc_core(void * p, size_t new_size)
{
    if (p == NULL)
        return lv_malloc_core(new_size);
    return rt_realloc(p, new_size);
}

void lv_free_core(void * p)
{
    rt_free(p);
}

void lv_mem_monitor_core(lv_mem_monitor_t * mon_p)
{
    /*Not supported*/
    LV_UNUSED(mon_p);
    return;
}

lv_result_t lv_mem_test_core(void)
{
    /*Not supported*/
    return LV_RESULT_OK;
}

/**********************
 *   STATIC FUNCTIONS
 **********************/

#endif /*LV_STDLIB_RTTHREAD*/
