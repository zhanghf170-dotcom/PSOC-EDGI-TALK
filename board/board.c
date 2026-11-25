/*
 * Copyright (c) 2006-2023, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2022-06-29     Rbb666       first version
 * 2025-08-20     Hydevcode
 */

#include "board.h"
#include "cy_sysclk.h"
#include "cycfg_peripheral_clocks.h"
#include "cy_cmsis_utils.h"

#define ES8388_CTRL             GET_PIN(16, 2)
#define SPEAKER_OE_CTRL         GET_PIN(21, 6)
#define WIFI_OE_CTRL            GET_PIN(16, 3)
#define WIFI_WL_REG_OE_CTRL     GET_PIN(11, 6)
#define CTRL                    GET_PIN(7, 2)

static void board_mpu_optimize(void)
{
    /* Match Non-Cacheable MPU windows to link.ld:
     *   - 0x26000000..0x2636FFFF (m55_code_secondary + m55_data_secondary): WB/WA cacheable via PRIVDEFENA
     *   - 0x26370000..0x263AFFFF (m33_m55_shared / UVC DMA): Non-Cacheable
     *   - 0x263B0000..0x264FFFFF (gfx_mem): Non-Cacheable
     *   - 0x64200000..0x645FFFFF (m55_hyperram heap): WB/WA cacheable via PRIVDEFENA
     *   - 0x64600000..0x64FFFFFF (m33_m55_shared_hyperram): Non-Cacheable
     */
    static const cy_stc_mpu_config_t mpu_opt_cfg[4] =
    {
        {
            .reg_num = 0U,
            .base_addr = 0x64600000U,
            .end_addr = 0x64FFFFFFU,
            .writable = true,
            .require_privileged = false,
            .shareable = false,
            .executable = false,
            .cacheable = 4U,
            .is_device = false,
            .device_attrs = 0U,
        },
        {
            .reg_num = 1U,
            .base_addr = 0x240FD000U,
            .end_addr = 0x240FFFFFU,
            .writable = true,
            .require_privileged = false,
            .shareable = false,
            .executable = false,
            .cacheable = 4U,
            .is_device = false,
            .device_attrs = 0U,
        },
        {
            .reg_num = 2U,
            .base_addr = 0x26370000U,
            .end_addr = 0x263AFFFFU,
            .writable = true,
            .require_privileged = false,
            .shareable = false,
            .executable = false,
            .cacheable = 4U,
            .is_device = false,
            .device_attrs = 0U,
        },
        {
            .reg_num = 3U,
            .base_addr = 0x263B0000U,
            .end_addr = 0x264FFFFFU,
            .writable = true,
            .require_privileged = false,
            .shareable = false,
            .executable = false,
            .cacheable = 4U,
            .is_device = false,
            .device_attrs = 0U,
        },
    };

#if defined(__DCACHE_PRESENT) && (__DCACHE_PRESENT == 1U)
    if ((SCB->CCR & SCB_CCR_DC_Msk) != 0U)
    {
        SCB_CleanInvalidateDCache();
    }
#endif
    Cy_MPU_Init(mpu_opt_cfg, (uint8_t)(sizeof(mpu_opt_cfg) / sizeof(mpu_opt_cfg[0])));
#if defined(__ICACHE_PRESENT) && (__ICACHE_PRESENT == 1U)
    if ((SCB->CCR & SCB_CCR_IC_Msk) != 0U)
    {
        SCB_InvalidateICache();
    }
#endif
}

void cy_bsp_all_init(void)
{
    cy_rslt_t result;

    /* Initialize the device and board peripherals */
    result = cybsp_init();

    /* Board init failed. Stop program execution */
    if (result != CY_RSLT_SUCCESS)
    {
        CY_ASSERT(0);
    }

    board_mpu_optimize();

#ifdef BSP_USING_SDIO0
    Cy_SysClk_PeriPclkEnableDivider(
        (en_clk_dst_t)CYBSP_SDHC_0_CLK_DIV_GRP_NUM,
        CYBSP_SDHC_0_CLK_DIV_HW,
        CYBSP_SDHC_0_CLK_DIV_NUM);
#endif

#ifdef BSP_USING_SDIO1
    /*
     * SDHC1 is assigned to M55. cybsp_init() already programmed its clock
     * divider via init_cycfg_peripheral_clocks(); we must also enable it.
     * Must run AFTER cybsp_init() — PERI1 power group is not up before.
     */
    Cy_SysClk_PeriPclkEnableDivider(
        (en_clk_dst_t)CYBSP_SDHC_1_CLK_DIV_GRP_NUM,
        CYBSP_SDHC_1_CLK_DIV_HW,
        CYBSP_SDHC_1_CLK_DIV_NUM);
#endif

}

void _start(void)
{
    extern int entry(void);
    entry();
    while (1);
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
    MSH_CMD_EXPORT(poweroff, The software enables the system to shut down. Simply press the button to restart it.);
#endif

//Mos管控制
#define ES8388_CTRL                 GET_PIN(16, 2)  //ES8388 电源 Enable引脚
#define SPEAKER_OE_CTRL             GET_PIN(21, 6)  //功放 Enable引脚
#define WIFI_OE_CTRL                GET_PIN(16, 3)  //WIFI Enable引脚
#define WIFI_WL_REG_OE_CTRL         GET_PIN(11, 6)  //WiFi寄存器开关
#define CTRL                        GET_PIN(7, 2)   //底板 3V3 DCDC电源控制
#define BL_PWM_DISP_CTRL            GET_PIN(20, 6)  //LCD PWM亮度调节
#define LCD_BL_GPIO_NUM             GET_PIN(15, 7)  //LCD 背光电源开关
#define LCD_DISP_GPIO_NUM           GET_PIN(15, 6)  //LCD IC电源开关
int en_gpio(void)
{
    rt_pin_mode(CTRL, PIN_MODE_OUTPUT);
    rt_pin_write(CTRL, PIN_HIGH);
    Cy_SysLib_Delay(50U);

    rt_pin_mode(WIFI_OE_CTRL, PIN_MODE_OUTPUT);
    rt_pin_write(WIFI_OE_CTRL, PIN_HIGH);

    rt_pin_mode(WIFI_WL_REG_OE_CTRL, PIN_MODE_OUTPUT);
    rt_pin_write(WIFI_WL_REG_OE_CTRL, PIN_HIGH);

    rt_pin_mode(ES8388_CTRL, PIN_MODE_OUTPUT);
    rt_pin_write(ES8388_CTRL, PIN_HIGH);

    rt_pin_mode(SPEAKER_OE_CTRL, PIN_MODE_OUTPUT);
    rt_pin_write(SPEAKER_OE_CTRL, PIN_HIGH);

    rt_pin_mode(BL_PWM_DISP_CTRL, PIN_MODE_OUTPUT);
    rt_pin_mode(LCD_DISP_GPIO_NUM, PIN_MODE_OUTPUT);
    rt_pin_mode(LCD_BL_GPIO_NUM, PIN_MODE_OUTPUT);
    rt_pin_write(BL_PWM_DISP_CTRL, PIN_HIGH);
    rt_pin_write(LCD_DISP_GPIO_NUM, PIN_HIGH);
    rt_pin_write(LCD_BL_GPIO_NUM, PIN_HIGH);

    return 0;
}
INIT_BOARD_EXPORT(en_gpio);
