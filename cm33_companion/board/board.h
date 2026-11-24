#ifndef __BOARD_H__
#define __BOARD_H__

#include <rtthread.h>
#include "drv_common.h"
#include "drv_gpio.h"

#include "cy_result.h"
#include "cybsp_types.h"
#include "mtb_hal.h"
#include "cybsp.h"

#define IFX_SRAM_SIZE   (256)
#define IFX_SRAM_END    (0x240BD000 + IFX_SRAM_SIZE * 1024)

extern unsigned int __end__;
extern unsigned int __HeapLimit;
#define HEAP_BEGIN      (void *)&__end__
#define HEAP_END        (void *)&__HeapLimit

void cy_bsp_all_init(void);

#endif
