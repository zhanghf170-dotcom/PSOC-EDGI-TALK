/*
 * Copyright (c) 2006-2025, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * CM33 Companion Board Init:
 *   Boots Cortex-M55 cleanly and continues running RT-Thread on CM33 without
 *   entering DeepSleep and without interfering with M55 external power rails.
 */

#include "board.h"

#define ES8388_CTRL         GET_PIN(16, 2)
#define SPEAKER_OE_CTRL     GET_PIN(21, 6)
#define WIFI_OE_CTRL        GET_PIN(16, 3)
#define WIFI_WL_REG_OE_CTRL GET_PIN(11, 6)
#define CTRL                GET_PIN(7, 2)

void cy_bsp_all_init(void)
{
    cy_rslt_t result;

    result = cybsp_init();
    if (result != CY_RSLT_SUCCESS)
    {
        CY_ASSERT(0);
    }

    if (CY_SYSLIB_RESET_HIB_WAKEUP == (Cy_SysLib_GetResetReason() &
                                       CY_SYSLIB_RESET_HIB_WAKEUP))
    {
        Cy_SysLib_ClearResetReason();
        Cy_SysPm_IoUnfreeze();
        Cy_SysPm_TriggerXRes();
    }

#ifdef SOC_Enable_CM55
    Cy_SysEnableCM55(MXCM55, CY_CM55_APP_BOOT_ADDR, 10);
#endif
}

void _start(void)
{
    extern int entry(void);
    entry();
    while (1)
    {
    }
    __builtin_unreachable();
}

void poweroff(void)
{
    rt_pin_mode(WIFI_OE_CTRL, PIN_MODE_OUTPUT);
    rt_pin_write(WIFI_OE_CTRL, PIN_LOW);

    rt_pin_mode(WIFI_WL_REG_OE_CTRL, PIN_MODE_OUTPUT);
    rt_pin_write(WIFI_WL_REG_OE_CTRL, PIN_LOW);

    rt_pin_mode(ES8388_CTRL, PIN_MODE_OUTPUT);
    rt_pin_write(ES8388_CTRL, PIN_LOW);

    rt_pin_mode(SPEAKER_OE_CTRL, PIN_MODE_OUTPUT);
    rt_pin_write(SPEAKER_OE_CTRL, PIN_LOW);

    rt_pin_mode(CTRL, PIN_MODE_OUTPUT);
    rt_pin_write(CTRL, PIN_LOW);

    Cy_SysClk_PllDisable(SRSS_DPLL_LP_0_PATH_NUM);
    Cy_SysPm_SystemEnterHibernate();
}

#ifdef RT_USING_MSH
MSH_CMD_EXPORT(poweroff, Shutdown system cleanly; press wake button to restart.);
#endif
