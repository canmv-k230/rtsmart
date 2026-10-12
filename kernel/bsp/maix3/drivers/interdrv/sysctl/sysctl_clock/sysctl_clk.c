/* Copyright (c) 2023, Canaan Bright Sight Co., Ltd
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND
 * CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <rtthread.h>
#include <rthw.h>
#include <stdlib.h>
#include <math.h>
#include "sysctl_boot.h"
#include "sysctl_clk.h"
#include "ioremap.h"
#include "board.h"
#include "tick.h"
#include "k230_pm_dev.h"

/* created by yangfan */
/* please refer to the sysctl_clk.h file for API description */

#define OSC_CLOCK_FREQ_24M (24000000)
#define TIMER_PULSE_IN (50000000)
#define SYSCTL_PLL_FSM_POWER_DOWN 0U
#define SYSCTL_PLL_FSM_READY 2U
#define SYSCTL_PLL_LOCK_MARGIN_US 1000U

extern volatile sysctl_boot_t *sysctl_boot;
volatile sysctl_clk_t* sysctl_clk = (volatile sysctl_clk_t*)SYSCTL_CLK_BASE_ADDR;

// volatile sysctl_clk_attr_t *sysctl_attr;

/* 判断clk的属性. */
int sysctl_clk_attribute(sysctl_clk_node_e clk)
{
    switch(clk)
    {
        /*--------------------------- ROOT CLOCK: OSC24M, PLL0-3 ------------------------------------*/
        case SYSCTL_CLK_OSC24M:
        case SYSCTL_CLK_ROOT_TIMERX_PULSE_IN:
        case SYSCTL_CLK_PLL0_OCLK:
        case SYSCTL_CLK_PLL0_OCLK_DIV2:
        case SYSCTL_CLK_PLL0_OCLK_DIV3:
        case SYSCTL_CLK_PLL0_OCLK_DIV4:
        case SYSCTL_CLK_PLL1_OCLK:
        case SYSCTL_CLK_PLL1_OCLK_DIV2:
        case SYSCTL_CLK_PLL1_OCLK_DIV3:
        case SYSCTL_CLK_PLL1_OCLK_DIV4:
        case SYSCTL_CLK_PLL2_OCLK:
        case SYSCTL_CLK_PLL2_OCLK_DIV2:
        case SYSCTL_CLK_PLL2_OCLK_DIV3:
        case SYSCTL_CLK_PLL2_OCLK_DIV4:
        case SYSCTL_CLK_PLL3_OCLK:
        case SYSCTL_CLK_PLL3_OCLK_DIV2:
        case SYSCTL_CLK_PLL3_OCLK_DIV3:
        case SYSCTL_CLK_PLL3_OCLK_DIV4:
        {
            return SYSCTL_READ_ENABLE | SYSCTL_WRITE_ENABLE;
        }

        /*--------------------------- CPU0 CLOCK ------------------------------------*/
        case SYSCTL_CLK_CPU0_CORE_CLK_ROOT:
        case SYSCTL_CLK_CPU0_CORE_CLK:
        case SYSCTL_CLK_CPU0_PLICLK:
        case SYSCTL_CLK_CPU0_ACLK:
        case SYSCTL_CLK_TDI_PCLK:
        case SYSCTL_CLK_CPU0_PCLK:
        {
            return SYSCTL_READ_ENABLE | SYSCTL_WRITE_ENABLE;
        }

        /*--------------------------- CPU1 CLOCK ------------------------------------*/
        case SYSCTL_CLK_CPU1_CORE_CLK_SRC:
        case SYSCTL_CLK_CPU1_CORE_CLK_ROOT:
        case SYSCTL_CLK_CPU1_CORE_CLK:
        case SYSCTL_CLK_CPU1_PLICLK:
        case SYSCTL_CLK_CPU1_ACLK:
        case SYSCTL_CLK_CPU1_PCLK:
        {
            return SYSCTL_READ_ENABLE | SYSCTL_WRITE_ENABLE;
        }

        /*--------------------------- AI CLOCK ------------------------------------*/
        case SYSCTL_CLK_AI_CORE_CLK:
        case SYSCTL_CLK_AI_ACLK:
        {
            return SYSCTL_READ_ENABLE | SYSCTL_WRITE_ENABLE;
        }

        /*--------------------------- VPU CLOCK ------------------------------------*/
        case SYSCTL_CLK_VPU_CLK_ROOT:
        case SYSCTL_CLK_VPU_CLK:
        case SYSCTL_CLK_VPU_ACLK_ROOT:
        case SYSCTL_CLK_NOC_VPU_ACLK:
        case SYSCTL_CLK_VPU_ACLK:
        case SYSCTL_CLK_NOC_VPU_CFG_CLK:
        case SYSCTL_CLK_VPU_CFG_CLK:
        {
            return SYSCTL_READ_ENABLE | SYSCTL_WRITE_ENABLE;
        }

        /*--------------------------- PMU CLOCK ------------------------------------*/
        case SYSCTL_CLK_PMU_PCLK:
        {
            return SYSCTL_READ_ENABLE | SYSCTL_WRITE_ENABLE;
        }

        /*--------------------------- HS CLOCK ------------------------------------*/
        case SYSCTL_CLK_NOC_HS_OSPI_HCLK:
        case SYSCTL_CLK_HS_HCLK_HIGH:
        case SYSCTL_CLK_NOC_HS_SUB_HCLK:
        case SYSCTL_CLK_HS_HCLK:
        case SYSCTL_CLK_SD0_HCLK:
        case SYSCTL_CLK_SD1_HCLK:
        case SYSCTL_CLK_USB0_HCLK:
        case SYSCTL_CLK_USB1_HCLK:
        case SYSCTL_CLK_SSI0_HCLK:
        case SYSCTL_CLK_SSI1_HCLK:
        case SYSCTL_CLK_SSI2_HCLK:

        case SYSCTL_CLK_NOC_HS_OSPI_ACLK:
        case SYSCTL_CLK_SSI0_ACLK:
        case SYSCTL_CLK_SSI1_CLK:
        case SYSCTL_CLK_SSI2_CLK:
        case SYSCTL_CLK_NOC_HS_QSPI_ACLK:
        case SYSCTL_CLK_QSPI_ACLK:
        case SYSCTL_CLK_SSI1_ACLK:
        case SYSCTL_CLK_SSI2_ACLK:

        case SYSCTL_CLK_SSI0_CLK:

        case SYSCTL_CLK_NOC_HS_SD_ACLK:
        case SYSCTL_CLK_SD_ACLK:
        case SYSCTL_CLK_SD0_ACLK:
        case SYSCTL_CLK_SD1_ACLK:
        case SYSCTL_CLK_SD_BCLK:
        case SYSCTL_CLK_SD0_BCLK:
        case SYSCTL_CLK_SD1_BCLK:

        case SYSCTL_CLK_SD_CCLK:
        case SYSCTL_CLK_SD0_CCLK_TX:
        case SYSCTL_CLK_SD1_CCLK_TX:

        case SYSCTL_CLK_PLL0_OCLK_DIV16:
        case SYSCTL_CLK_USB_REFCLK_50M:
        case SYSCTL_CLK_USB0_REFCLK:
        case SYSCTL_CLK_USB1_REFCLK:

        case SYSCTL_CLK_SD_TMCLK:
        case SYSCTL_CLK_SD0_TMCLK:
        case SYSCTL_CLK_SD1_TMCLK:
        {
            return SYSCTL_READ_ENABLE | SYSCTL_WRITE_ENABLE;
        }

        /*--------------------------- LS CLOCK ------------------------------------*/
        case SYSCTL_CLK_NOC_LS_PCLK:
        case SYSCTL_CLK_LS_PCLK:
        case SYSCTL_CLK_UART0_PCLK:
        case SYSCTL_CLK_UART1_PCLK:
        case SYSCTL_CLK_UART2_PCLK:
        case SYSCTL_CLK_UART3_PCLK:
        case SYSCTL_CLK_UART4_PCLK:
        case SYSCTL_CLK_I2C0_PCLK:
        case SYSCTL_CLK_I2C1_PCLK:
        case SYSCTL_CLK_I2C2_PCLK:
        case SYSCTL_CLK_I2C3_PCLK:
        case SYSCTL_CLK_I2C4_PCLK:
        case SYSCTL_CLK_GPIO_PCLK:
        case SYSCTL_CLK_PWM_PCLK:
        case SYSCTL_CLK_JAMLINK0_PCLK:
        case SYSCTL_CLK_JAMLINK1_PCLK:
        case SYSCTL_CLK_JAMLINK2_PCLK:
        case SYSCTL_CLK_JAMLINK3_PCLK:
        case SYSCTL_CLK_ADC_PCLK:
        case SYSCTL_CLK_AUDIO_PCLK:
        case SYSCTL_CLK_CODEC_PCLK:

        case SYSCTL_CLK_UART0_CLK:
        case SYSCTL_CLK_UART1_CLK:
        case SYSCTL_CLK_UART2_CLK:
        case SYSCTL_CLK_UART3_CLK:
        case SYSCTL_CLK_UART4_CLK:

        case SYSCTL_CLK_JAMLINK0_CO:
        case SYSCTL_CLK_JAMLINK1_CO:
        case SYSCTL_CLK_JAMLINK2_CO:
        case SYSCTL_CLK_JAMLINK3_CO:

        case SYSCTL_CLK_I2C0_CLK:
        case SYSCTL_CLK_I2C1_CLK:
        case SYSCTL_CLK_I2C2_CLK:
        case SYSCTL_CLK_I2C3_CLK:
        case SYSCTL_CLK_I2C4_CLK:

        case SYSCTL_CLK_ADC_CLK:
        case SYSCTL_CLK_GPIO_DBCLK:
        {
            return SYSCTL_READ_ENABLE | SYSCTL_WRITE_ENABLE;
        }

        /*--------------------------- AUDIO CLOCK ------------------------------------*/
        case SYSCTL_CLK_CODEC_ADC_MCLK:
        case SYSCTL_CLK_CODEC_DAC_MCLK:
        case SYSCTL_CLK_AUD_DEV_CLK:
        case SYSCTL_CLK_PDM_CLK_ROOT:
        {
            return SYSCTL_READ_ENABLE | SYSCTL_WRITE_ENABLE;
        }

        /*--------------------------- SYSCTL CLOCK ------------------------------------*/
        case SYSCTL_CLK_SYSCTL_PCLK:
        case SYSCTL_CLK_WDT0_PCLK:
        case SYSCTL_CLK_WDT1_PCLK:
        case SYSCTL_CLK_TIMER_PCLK:
        case SYSCTL_CLK_IOMUX_PCLK:
        case SYSCTL_CLK_MAILBOX_PCLK:

        case SYSCTL_CLK_HDI_CLK:

        case SYSCTL_CLK_STC_CLK:

        case SYSCTL_CLK_TS_CLK:

        case SYSCTL_CLK_WDT0_CLK:
        case SYSCTL_CLK_WDT1_CLK:
        {
            return SYSCTL_READ_ENABLE | SYSCTL_WRITE_ENABLE;
        }

        /*--------------------------- TIMER CLOCK ------------------------------------*/
        case SYSCTL_CLK_TIMERX_PULSE_IN:
        case SYSCTL_CLK_TIMER0_CLK:
        case SYSCTL_CLK_TIMER1_CLK:
        case SYSCTL_CLK_TIMER2_CLK:
        case SYSCTL_CLK_TIMER3_CLK:
        case SYSCTL_CLK_TIMER4_CLK:
        case SYSCTL_CLK_TIMER5_CLK:
        {
            return SYSCTL_READ_ENABLE | SYSCTL_WRITE_ENABLE;
        }

        /*--------------------------- SHRM CLOCK ------------------------------------*/
        case SYSCTL_CLK_SRAM_CLK:
        case SYSCTL_CLK_SHRM_DIV2:
        case SYSCTL_CLK_SHRM_AXIM_CLK:
        case SYSCTL_CLK_SHRM_AXIS_CLK:
        case SYSCTL_CLK_DECOMP_PCLK:
        case SYSCTL_CLK_DECOMP_CLK:

        case SYSCTL_CLK_NOC_SHRM_PCLK:
        case SYSCTL_CLK_SHRM_PCLK:

        case SYSCTL_CLK_NOC_SHRM_MCLK:
        case SYSCTL_CLK_GSDMA_PCLK:
        case SYSCTL_CLK_GSDMA_CLK:
        case SYSCTL_CLK_NOAI_2D_PCLK:
        case SYSCTL_CLK_NOAI_2D_CLK:
        case SYSCTL_CLK_PDMA_PCLK:
        case SYSCTL_CLK_PDMA_CLK:
        {
            return SYSCTL_READ_ENABLE | SYSCTL_WRITE_ENABLE;
        }

        /*--------------------------- DDR CLOCK ------------------------------------*/
        case SYSCTL_CLK_DDRC_CORE_CLK:
        case SYSCTL_CLK_DDR_BYPASSPCLK:
        case SYSCTL_CLK_NOC_DDRC_PCLK:
        case SYSCTL_CLK_DDRC_PCLK:
        case SYSCTL_CLK_DDRC_P0_CLK:
        case SYSCTL_CLK_NOC_DDRC_P4_CLK:
        case SYSCTL_CLK_NOC_DDRC_P3_ACLK:
        case SYSCTL_CLK_NOC_DDRC_P2_ACLK:
        case SYSCTL_CLK_NOC_DDRC_P1_ACLK:
        {
            return SYSCTL_READ_ENABLE | SYSCTL_WRITE_ENABLE;
        }

        /*--------------------------- ISP CLOCK ------------------------------------*/
        case SYSCTL_CLK_ISP_CFG_CLK:
        case SYSCTL_CLK_ISP_PCLK:
        case SYSCTL_CLK_CSI0_CFG_CLK:
        case SYSCTL_CLK_CSI1_CFG_CLK:
        case SYSCTL_CLK_CSI2_CFG_CLK:
        case SYSCTL_CLK_ISP_DPHY_PCLK:
        case SYSCTL_CLK_CSI0_PIXEL_CLK:
        case SYSCTL_CLK_CSI1_PIXEL_CLK:
        case SYSCTL_CLK_CSI2_PIXEL_CLK:
        case SYSCTL_CLK_CSI0_PCLK:
        case SYSCTL_CLK_CSI1_PCLK:
        case SYSCTL_CLK_CSI2_PCLK:
        case SYSCTL_CLK_ISP_CLK:
        case SYSCTL_CLK_ISP_MCM_CLK:
        case SYSCTL_CLK_NOC_ISP_HCLK:
        case SYSCTL_CLK_ISP_HCLK:
        case SYSCTL_CLK_NOC_ISP_ACLK:
        case SYSCTL_CLK_ISP_ACLK:
        case SYSCTL_CLK_ISPDWE_CLK:
        case SYSCTL_CLK_ISPVSE_CLK:
        case SYSCTL_CLK_ISP_DW_HCLK:
        case SYSCTL_CLK_ISP_DW_ACLK:
        {
            return SYSCTL_READ_ENABLE | SYSCTL_WRITE_ENABLE;
        }

        /*--------------------------- MCLK CLOCK ------------------------------------*/
        case SYSCTL_CLK_MCLK0:
        case SYSCTL_CLK_MCLK1:
        case SYSCTL_CLK_MCLK2:
        {
            return SYSCTL_READ_ENABLE | SYSCTL_WRITE_ENABLE;
        }

        /*--------------------------- DPU CLOCK ------------------------------------*/
        case SYSCTL_CLK_DPU_CLK:
        case SYSCTL_CLK_NOC_DPU_ACLK:
        case SYSCTL_CLK_DPU_ACLK:
        case SYSCTL_CLK_NOC_DPU_PCLK:
        case SYSCTL_CLK_DPU_PCLK:
        {
            return SYSCTL_READ_ENABLE | SYSCTL_WRITE_ENABLE;
        }

        /*--------------------------- DISP CLOCK ------------------------------------*/
        case SYSCTL_CLK_NOC_DISP_HCLK:
        case SYSCTL_CLK_DISP_HCLK:
        case SYSCTL_CLK_NOC_DISP_ACLK:
        case SYSCTL_CLK_DISP_ACLK:
        case SYSCTL_CLK_DISP_CLKEXT:
        case SYSCTL_CLK_CLK_GPU:
        case SYSCTL_CLK_DPIPCLK:
        case SYSCTL_CLK_DISP_CFGCLK:
        case SYSCTL_CLK_DISP_REFCLK:
        case SYSCTL_CLK_DISP_VO_PCLK:
        case SYSCTL_CLK_DISP_DSI_PCLK:
        case SYSCTL_CLK_DISP_DSI_PHY_PCLK:
        case SYSCTL_CLK_DISP_GPU_HCLK:
        case SYSCTL_CLK_DISP_VO_ACLK:
        case SYSCTL_CLK_DISP_GPU_ACLK:
        {
            return SYSCTL_READ_ENABLE | SYSCTL_WRITE_ENABLE;
        }

        /*--------------------------- SEC CLOCK ------------------------------------*/
        case SYSCTL_CLK_NOC_SEC_PCLK:
        case SYSCTL_CLK_SEC_PCLK:
        case SYSCTL_CLK_FIXCLK:

        case SYSCTL_CLK_NOC_SEC_ACLK:
        case SYSCTL_CLK_SEC_ACLK:
        {
            return SYSCTL_READ_ENABLE | SYSCTL_WRITE_ENABLE;
        }

        /*--------------------------- USB TEST MODE CLOCK ------------------------------------*/
        case SYSCTL_CLK_USB_TEST_CLK_GATE:
        case SYSCTL_CLK_USB_CLK480:
        case SYSCTL_CLK_USB_CLK100:
        {
            return SYSCTL_READ_ENABLE | SYSCTL_WRITE_ENABLE;
        }

        /*--------------------------- DPHY DFT MODE CLOCK ------------------------------------*/
        case SYSCTL_CLK_DPHY_TEST_CLK:
        {
            return SYSCTL_READ_ENABLE | SYSCTL_WRITE_ENABLE;
        }

        /*--------------------------- SPI2AXI CLOCK ------------------------------------*/
        case SYSCTL_CLK_NOC_SPI2AXI_ACLK:
        case SYSCTL_CLK_SPI2AXI_ACLK:
        {
            return SYSCTL_READ_ENABLE | SYSCTL_WRITE_ENABLE;
        }

        default:
            return SYSCTL_READ_ENABLE | SYSCTL_WRITE_ENABLE;
    }
}

/***************************************************************************
*
* API of root node
*
**************************************************************************/
/* if PLL bypass, the output clock is 24m clock. If there is no bypass, the clock comes from PLL */
bool sysctl_boot_get_root_clk_bypass(sysctl_clk_node_e clk)
{
    switch(clk)
    {
        case SYSCTL_CLK_PLL0_OCLK:
        case SYSCTL_CLK_PLL0_OCLK_DIV2:
        case SYSCTL_CLK_PLL0_OCLK_DIV3:
        case SYSCTL_CLK_PLL0_OCLK_DIV4:
            return ((sysctl_boot->pll[0].cfg1 >> 19) & 0x1) ? true:false;

        case SYSCTL_CLK_PLL1_OCLK:
        case SYSCTL_CLK_PLL1_OCLK_DIV2:
        case SYSCTL_CLK_PLL1_OCLK_DIV3:
        case SYSCTL_CLK_PLL1_OCLK_DIV4:
            return ((sysctl_boot->pll[1].cfg1 >> 19) & 0x1) ? true:false;

        case SYSCTL_CLK_PLL2_OCLK:
        case SYSCTL_CLK_PLL2_OCLK_DIV2:
        case SYSCTL_CLK_PLL2_OCLK_DIV3:
        case SYSCTL_CLK_PLL2_OCLK_DIV4:
            return ((sysctl_boot->pll[2].cfg1 >> 19) & 0x1) ? true:false;

        case SYSCTL_CLK_PLL3_OCLK:
        case SYSCTL_CLK_PLL3_OCLK_DIV2:
        case SYSCTL_CLK_PLL3_OCLK_DIV3:
        case SYSCTL_CLK_PLL3_OCLK_DIV4:
            return ((sysctl_boot->pll[3].cfg1 >> 19) & 0x1) ? true:false;

        default:
            return false;
    }
}

/* if PLL bypass, the output clock is 24m clock. If there is no bypass, the clock comes from PLL */
void sysctl_boot_set_root_clk_bypass(sysctl_clk_node_e clk, bool enable)
{
    volatile uint32_t ret;

    switch(clk)
    {
        case SYSCTL_CLK_PLL0_OCLK:
        case SYSCTL_CLK_PLL0_OCLK_DIV2:
        case SYSCTL_CLK_PLL0_OCLK_DIV3:
        case SYSCTL_CLK_PLL0_OCLK_DIV4:
            if(true == enable)  /* enable bypass */
            {
                sysctl_boot->pll[0].cfg1 |= (1 << 19);
            }
            else
            {
                sysctl_boot->pll[0].cfg1 &= ~(1 << 19);
            }
            return;

        case SYSCTL_CLK_PLL1_OCLK:
        case SYSCTL_CLK_PLL1_OCLK_DIV2:
        case SYSCTL_CLK_PLL1_OCLK_DIV3:
        case SYSCTL_CLK_PLL1_OCLK_DIV4:
            if(true == enable)  /* enable bypass */
            {
                sysctl_boot->pll[1].cfg1 |= (1 << 19);
            }
            else
            {
                sysctl_boot->pll[1].cfg1 &= ~(1 << 19);
            }
            return;

        case SYSCTL_CLK_PLL2_OCLK:
        case SYSCTL_CLK_PLL2_OCLK_DIV2:
        case SYSCTL_CLK_PLL2_OCLK_DIV3:
        case SYSCTL_CLK_PLL2_OCLK_DIV4:
            if(true == enable)  /* enable bypass */
            {
                sysctl_boot->pll[2].cfg1 |= (1 << 19);
            }
            else
            {
                sysctl_boot->pll[2].cfg1 &= ~(1 << 19);
            }
            return;

        case SYSCTL_CLK_PLL3_OCLK:
        case SYSCTL_CLK_PLL3_OCLK_DIV2:
        case SYSCTL_CLK_PLL3_OCLK_DIV3:
        case SYSCTL_CLK_PLL3_OCLK_DIV4:
            if(true == enable)  /* enable bypass */
            {
                sysctl_boot->pll[3].cfg1 |= (1 << 19);
            }
            else
            {
                sysctl_boot->pll[3].cfg1 &= ~(1 << 19);
            }
            return;

        default:
            return;
    }
}

static int sysctl_boot_root_pll_index(sysctl_clk_node_e clk)
{
    switch (clk) {
    case SYSCTL_CLK_PLL0_OCLK:
    case SYSCTL_CLK_PLL0_OCLK_DIV2:
    case SYSCTL_CLK_PLL0_OCLK_DIV3:
    case SYSCTL_CLK_PLL0_OCLK_DIV4:
        return 0;
    case SYSCTL_CLK_PLL1_OCLK:
    case SYSCTL_CLK_PLL1_OCLK_DIV2:
    case SYSCTL_CLK_PLL1_OCLK_DIV3:
    case SYSCTL_CLK_PLL1_OCLK_DIV4:
        return 1;
    case SYSCTL_CLK_PLL2_OCLK:
    case SYSCTL_CLK_PLL2_OCLK_DIV2:
    case SYSCTL_CLK_PLL2_OCLK_DIV3:
    case SYSCTL_CLK_PLL2_OCLK_DIV4:
        return 2;
    case SYSCTL_CLK_PLL3_OCLK:
    case SYSCTL_CLK_PLL3_OCLK_DIV2:
    case SYSCTL_CLK_PLL3_OCLK_DIV3:
    case SYSCTL_CLK_PLL3_OCLK_DIV4:
        return 3;
    default:
        return -1;
    }
}

static uint32_t sysctl_boot_root_pll_fsm(unsigned int id)
{
    return (sysctl_boot->pll[id].state >> 4) & 0x3U;
}

static bool sysctl_boot_root_pll_ready(unsigned int id)
{
    return (sysctl_boot->pll[id].state & 0x1U) != 0U &&
        sysctl_boot_root_pll_fsm(id) == SYSCTL_PLL_FSM_READY;
}

/*
   get enable/disable state of PLL output clock
*/
bool sysctl_boot_get_root_clk_en(sysctl_clk_node_e clk)
{
    switch(clk)
    {
        case SYSCTL_CLK_PLL0_OCLK:
        case SYSCTL_CLK_PLL0_OCLK_DIV2:
        case SYSCTL_CLK_PLL0_OCLK_DIV3:
        case SYSCTL_CLK_PLL0_OCLK_DIV4:
            return ((sysctl_boot->pll[0].ctl >> 2) & 0x1) ? true:false;

        case SYSCTL_CLK_PLL1_OCLK:
        case SYSCTL_CLK_PLL1_OCLK_DIV2:
        case SYSCTL_CLK_PLL1_OCLK_DIV3:
        case SYSCTL_CLK_PLL1_OCLK_DIV4:
            return ((sysctl_boot->pll[1].ctl >> 2) & 0x1) ? true:false;

        case SYSCTL_CLK_PLL2_OCLK:
        case SYSCTL_CLK_PLL2_OCLK_DIV2:
        case SYSCTL_CLK_PLL2_OCLK_DIV3:
        case SYSCTL_CLK_PLL2_OCLK_DIV4:
            return ((sysctl_boot->pll[2].ctl >> 2) & 0x1) ? true:false;

        case SYSCTL_CLK_PLL3_OCLK:
        case SYSCTL_CLK_PLL3_OCLK_DIV2:
        case SYSCTL_CLK_PLL3_OCLK_DIV3:
        case SYSCTL_CLK_PLL3_OCLK_DIV4:
            return ((sysctl_boot->pll[3].ctl >> 2) & 0x1) ? true:false;

        default:
            return false;
    }
}

/*
   Enable PLL output clock
*/
void sysctl_boot_set_root_clk_en(sysctl_clk_node_e clk, bool enable)
{
    switch(clk)
    {
        case SYSCTL_CLK_PLL0_OCLK:
        case SYSCTL_CLK_PLL0_OCLK_DIV2:
        case SYSCTL_CLK_PLL0_OCLK_DIV3:
        case SYSCTL_CLK_PLL0_OCLK_DIV4:
            if(true == enable)
            {
                sysctl_boot->pll[0].ctl |= (1 << 2) | (1 << 18);     /* enable pll */
            }

            return;

        case SYSCTL_CLK_PLL1_OCLK:
        case SYSCTL_CLK_PLL1_OCLK_DIV2:
        case SYSCTL_CLK_PLL1_OCLK_DIV3:
        case SYSCTL_CLK_PLL1_OCLK_DIV4:
            if(true == enable)
            {
                sysctl_boot->pll[1].ctl |= (1 << 2) | (1 << 18);     /* enable pll */
            }

            return;

        case SYSCTL_CLK_PLL2_OCLK:
        case SYSCTL_CLK_PLL2_OCLK_DIV2:
        case SYSCTL_CLK_PLL2_OCLK_DIV3:
        case SYSCTL_CLK_PLL2_OCLK_DIV4:
            if(true == enable)
            {
                sysctl_boot->pll[2].ctl |= (1 << 2) | (1 << 18);     /* enable pll */
            }

            return;

        case SYSCTL_CLK_PLL3_OCLK:
        case SYSCTL_CLK_PLL3_OCLK_DIV2:
        case SYSCTL_CLK_PLL3_OCLK_DIV3:
        case SYSCTL_CLK_PLL3_OCLK_DIV4:
            if(true == enable)
            {
                sysctl_boot->pll[3].ctl |= (1 << 2) | (1 << 18);     /* enable pll */
            }

            return;

        default:
            return;
    }
}

bool sysctl_boot_start_root_clk(sysctl_clk_node_e clk)
{
    int id = sysctl_boot_root_pll_index(clk);
    uint32_t lock_time_us;
    uint64_t start_us;

    if (id < 0)
        return false;

    if (sysctl_boot_root_pll_ready((unsigned int)id)) {
        sysctl_boot_set_root_clk_en(clk, true);
        return true;
    }

    /* pll_init is W1T. Hardware powers the PLL up, waits for lock, and
     * enables its output as part of the initialization state machine.
     */
    sysctl_boot->pll[id].ctl |= (1U << 1) | (1U << 17);

    lock_time_us = ((((sysctl_boot->pll[id].ctl >> 4) & 0xffU) + 1U) *
        32U) + SYSCTL_PLL_LOCK_MARGIN_US;
    start_us = cpu_ticks_us();
    do {
        if (sysctl_boot_root_pll_ready((unsigned int)id)) {
            sysctl_boot_set_root_clk_en(clk, true);
            return true;
        }
    } while (cpu_ticks_us() - start_us < lock_time_us);

    return false;
}

/* PLL power supply */
bool sysctl_boot_set_root_clk_pwroff(sysctl_clk_node_e clk)
{
    int id = sysctl_boot_root_pll_index(clk);
    uint64_t start_us;

    if (id < 0)
        return false;

    sysctl_boot->pll[id].ctl |= (1U << 0) | (1U << 16);
    start_us = cpu_ticks_us();
    do {
        if (sysctl_boot_root_pll_fsm((unsigned int)id) ==
            SYSCTL_PLL_FSM_POWER_DOWN)
            return true;
    } while (cpu_ticks_us() - start_us < SYSCTL_PLL_LOCK_MARGIN_US);

    return false;
}

/* check the lock state of PLL */
bool sysctl_boot_get_root_clk_lock(sysctl_clk_node_e clk)
{
    switch(clk)
    {
        case SYSCTL_CLK_PLL0_OCLK:
        case SYSCTL_CLK_PLL0_OCLK_DIV2:
        case SYSCTL_CLK_PLL0_OCLK_DIV3:
        case SYSCTL_CLK_PLL0_OCLK_DIV4:
            return ((sysctl_boot->pll[0].state >> 0) & 0x1) ? true:false;

        case SYSCTL_CLK_PLL1_OCLK:
        case SYSCTL_CLK_PLL1_OCLK_DIV2:
        case SYSCTL_CLK_PLL1_OCLK_DIV3:
        case SYSCTL_CLK_PLL1_OCLK_DIV4:
            return ((sysctl_boot->pll[1].state >> 0) & 0x1) ? true:false;

        case SYSCTL_CLK_PLL2_OCLK:
        case SYSCTL_CLK_PLL2_OCLK_DIV2:
        case SYSCTL_CLK_PLL2_OCLK_DIV3:
        case SYSCTL_CLK_PLL2_OCLK_DIV4:
            return ((sysctl_boot->pll[2].state >> 0) & 0x1) ? true:false;

        case SYSCTL_CLK_PLL3_OCLK:
        case SYSCTL_CLK_PLL3_OCLK_DIV2:
        case SYSCTL_CLK_PLL3_OCLK_DIV3:
        case SYSCTL_CLK_PLL3_OCLK_DIV4:
            return ((sysctl_boot->pll[3].state >> 0) & 0x1) ? true:false;

        default:
            return true;
    }
}

/* get PLL output frequency.
*  freq = (double)OSC_CLOCK_FREQ_24M * (double)(fbdiv+1) / (double)(refdiv+1) / (double)(outdiv+1)
*/
uint32_t sysctl_boot_get_root_clk_freq(sysctl_clk_node_e clk)
{
    uint32_t refdiv;    /* reference clock divide */
    uint32_t outdiv;    /* output clock divide */
    uint32_t fbdiv;     /* feedback clock divide */
    uint32_t freq;

    switch(clk)
    {
        case SYSCTL_CLK_OSC24M:
            return OSC_CLOCK_FREQ_24M;  /* 24MHz */

        case SYSCTL_CLK_ROOT_TIMERX_PULSE_IN:
            return TIMER_PULSE_IN;      /* 50MHz */

        case SYSCTL_CLK_PLL0_OCLK:
        case SYSCTL_CLK_PLL0_OCLK_DIV2:
        case SYSCTL_CLK_PLL0_OCLK_DIV3:
        case SYSCTL_CLK_PLL0_OCLK_DIV4:
        {
            if(true == sysctl_boot_get_root_clk_bypass(clk))
            {
                freq = OSC_CLOCK_FREQ_24M;
            }
            else
            {
                refdiv = (sysctl_boot->pll[0].cfg0 >> 16) & 0x3F;    /* bit 16~21 */
                outdiv = (sysctl_boot->pll[0].cfg0 >> 24) & 0xF;     /* bit 24~27 */
                fbdiv  = (sysctl_boot->pll[0].cfg0 >> 0)  & 0x1FFF;   /* bit 0~12 */
                freq = (double)OSC_CLOCK_FREQ_24M * (double)(fbdiv+1) / (double)(refdiv+1) / (double)(outdiv+1);
            }
            switch(clk)
            {
                case SYSCTL_CLK_PLL0_OCLK:
                    return freq;
                case SYSCTL_CLK_PLL0_OCLK_DIV2:
                    return freq/2;
                case SYSCTL_CLK_PLL0_OCLK_DIV3:
                    return freq/3;
                default:
                    return freq/4;
            }
        }

        case SYSCTL_CLK_PLL1_OCLK:
        case SYSCTL_CLK_PLL1_OCLK_DIV2:
        case SYSCTL_CLK_PLL1_OCLK_DIV3:
        case SYSCTL_CLK_PLL1_OCLK_DIV4:
        {
            if(true == sysctl_boot_get_root_clk_bypass(clk))
            {
                freq = OSC_CLOCK_FREQ_24M;
            }
            else
            {
                refdiv = (sysctl_boot->pll[1].cfg0 >> 16) & 0x3F;    /* bit 16~21 */
                outdiv = (sysctl_boot->pll[1].cfg0 >> 24) & 0xF;     /* bit 24~27 */
                fbdiv  = (sysctl_boot->pll[1].cfg0 >> 0)  & 0x1FFF;   /* bit 0~12 */
                freq = (double)OSC_CLOCK_FREQ_24M * (double)(fbdiv+1) / (double)(refdiv+1) / (double)(outdiv+1);
            }
            switch(clk)
            {
                case SYSCTL_CLK_PLL1_OCLK:
                    return freq;
                case SYSCTL_CLK_PLL1_OCLK_DIV2:
                    return freq/2;
                case SYSCTL_CLK_PLL1_OCLK_DIV3:
                    return freq/3;
                default:
                    return freq/4;
            }
        }

        case SYSCTL_CLK_PLL2_OCLK:
        case SYSCTL_CLK_PLL2_OCLK_DIV2:
        case SYSCTL_CLK_PLL2_OCLK_DIV3:
        case SYSCTL_CLK_PLL2_OCLK_DIV4:
        {
            if(true == sysctl_boot_get_root_clk_bypass(clk))
            {
                freq = OSC_CLOCK_FREQ_24M;
            }
            else
            {
                refdiv = (sysctl_boot->pll[2].cfg0 >> 16) & 0x3F;    /* bit 16~21 */
                outdiv = (sysctl_boot->pll[2].cfg0 >> 24) & 0xF;     /* bit 24~27 */
                fbdiv  = (sysctl_boot->pll[2].cfg0 >> 0)  & 0x1FFF;   /* bit 0~12 */
                freq = (double)OSC_CLOCK_FREQ_24M * (double)(fbdiv+1) / (double)(refdiv+1) / (double)(outdiv+1);
            }
            switch(clk)
            {
                case SYSCTL_CLK_PLL2_OCLK:
                    return freq;
                case SYSCTL_CLK_PLL2_OCLK_DIV2:
                    return freq/2;
                case SYSCTL_CLK_PLL2_OCLK_DIV3:
                    return freq/3;
                default:
                    return freq/4;
            }
        }

        case SYSCTL_CLK_PLL3_OCLK:
        case SYSCTL_CLK_PLL3_OCLK_DIV2:
        case SYSCTL_CLK_PLL3_OCLK_DIV3:
        case SYSCTL_CLK_PLL3_OCLK_DIV4:
        {
            if(true == sysctl_boot_get_root_clk_bypass(clk))
            {
                freq = OSC_CLOCK_FREQ_24M;
            }
            else
            {
                refdiv = (sysctl_boot->pll[3].cfg0 >> 16) & 0x3F;    /* bit 16~21 */
                outdiv = (sysctl_boot->pll[3].cfg0 >> 24) & 0xF;     /* bit 24~27 */
                fbdiv  = (sysctl_boot->pll[3].cfg0 >> 0)  & 0x1FFF;   /* bit 0~12 */
                freq = (double)OSC_CLOCK_FREQ_24M * (double)(fbdiv+1) / (double)(refdiv+1) / (double)(outdiv+1);
            }
            switch(clk)
            {
                case SYSCTL_CLK_PLL3_OCLK:
                    return freq;
                case SYSCTL_CLK_PLL3_OCLK_DIV2:
                    return freq/2;
                case SYSCTL_CLK_PLL3_OCLK_DIV3:
                    return freq/3;
                default:
                    return freq/4;
            }
        }

        default:
            return 0;
    }
}

/* 设置锁相环的频率。
  please configure the PLL frequency according to the above frequency division coefficient
  Note: when configuring, you can't configure yourself. For example, the PLL attached to CPU can't stop
  before configuring the PLL of CPU, switch the clock first and then configure it. After configuration, switch it back
*/
bool sysctl_boot_set_root_clk_freq(sysctl_clk_node_e clk, uint32_t fbdiv, uint32_t refdiv, uint32_t outdiv, uint32_t bwadj)
{
    uint32_t id;
    volatile uint32_t ret;

    if(SYSCTL_CLK_PLL0_OCLK == clk)
        id = 0;
    else if(SYSCTL_CLK_PLL1_OCLK == clk)
        id = 1;
    else if(SYSCTL_CLK_PLL2_OCLK == clk)
        id = 2;
    else if(SYSCTL_CLK_PLL3_OCLK == clk)
        id = 3;
    else
        return false;

    /* 根据k230 TRM手册，PLL的配置步骤如下：
        1. 配置PLLx_CTL.pllx_pwrdwn=1，关闭PLL的output；
        2. 配置PLL的参数PLLx_CFG0和PLLx_CFG1；
        3. 配置PLLx_CTL.pllx_init，硬件会等待PLL自动锁定并打开PLL的output。

    */
    /*1. poweroff pll*/
    if (!sysctl_boot_set_root_clk_pwroff(clk))
        return false;

    /*2. config divide*/
    sysctl_boot->pll[id].cfg0 = ((fbdiv &  0x1FFF) << 0) | ((refdiv &  0x3F) << 16) | ((outdiv & 0xF) << 24);
    ret = sysctl_boot->pll[id].cfg1;
    ret &= 0xfffff000;
    sysctl_boot->pll[id].cfg1 = ret | ((bwadj & 0xfff) << 0);

    /* 3. init pll. init will pwrup pll and wait for the ready state. */
    return sysctl_boot_start_root_clk(clk);
}


/***************************************************************************
*
* API of trunk and leaf node
*
**************************************************************************/
bool sysctl_clk_set_leaf_parent(sysctl_clk_node_e leaf, sysctl_clk_node_e parent)
{
    volatile uint32_t ret;

    switch(sysctl_clk_attribute(leaf))
    {
        case 0:
        case 1:
            return false;
        case 3:
            break;
    }

    switch(leaf)
    {
        /*--------------------------- CPU0 CLOCK ------------------------------------*/
        case SYSCTL_CLK_CPU0_CORE_CLK_ROOT:
        case SYSCTL_CLK_CPU0_PLICLK:
        case SYSCTL_CLK_CPU0_ACLK:
            return false;       /* always pll0_div2 */
        case SYSCTL_CLK_TDI_PCLK:
        case SYSCTL_CLK_CPU0_PCLK:
            return false;       /* always pll0_div4 */


        /*--------------------------- CPU1 CLOCK ------------------------------------*/
        case SYSCTL_CLK_CPU1_CORE_CLK_SRC:
        {
            /* CPU1_CLK_CFG[2:1]: 0=PLL1/2, 1=PLL3, 2=PLL0. */
            if(SYSCTL_CLK_PLL1_OCLK_DIV2 == parent)
            {
                ret = sysctl_clk->cpu1_clk_cfg;
                ret &= 0xfffffff9;
                sysctl_clk->cpu1_clk_cfg = ret | (0 << 1);
                return true;
            }
            else if(SYSCTL_CLK_PLL3_OCLK == parent)
            {
                ret = sysctl_clk->cpu1_clk_cfg;
                ret &= 0xfffffff9;
                sysctl_clk->cpu1_clk_cfg = ret | (1 << 1);
                return true;
            }
            else if(SYSCTL_CLK_PLL0_OCLK == parent)
            {
                ret = sysctl_clk->cpu1_clk_cfg;
                ret &= 0xfffffff9;
                sysctl_clk->cpu1_clk_cfg = ret | (2 << 1);
                return true;
            }
            else
            {
                return false;
            }
        }
        case SYSCTL_CLK_CPU1_PLICLK:
        case SYSCTL_CLK_CPU1_ACLK:
            return false;
        case SYSCTL_CLK_CPU1_PCLK:
            return false;       /* always pll0_div4 */

        /*--------------------------- AI CLOCK ------------------------------------*/
        case SYSCTL_CLK_AI_CORE_CLK:
        {
            /* AI_CLK_CFG[2]: 0=PLL1/2, 1=PLL3/2. */
            if(SYSCTL_CLK_PLL1_OCLK_DIV2 == parent)
            {
                ret = sysctl_clk->ai_clk_cfg;
                ret &= 0xfffffffb;
                sysctl_clk->ai_clk_cfg = ret | (0 << 2);
                return true;
            }
            else if(SYSCTL_CLK_PLL3_OCLK_DIV2 == parent)
            {
                ret = sysctl_clk->ai_clk_cfg;
                ret &= 0xfffffffb;
                sysctl_clk->ai_clk_cfg = ret | (1 << 2);
                return true;
            }
            else
            {
                return false;
            }
        }
        case SYSCTL_CLK_AI_ACLK:
            return false;       /* always pll0_div4 */

        /*--------------------------- VPU CLOCK ------------------------------------*/
        case SYSCTL_CLK_VPU_CLK_ROOT:
        case SYSCTL_CLK_VPU_ACLK_ROOT:
        case SYSCTL_CLK_VPU_ACLK:
            return false;
        case SYSCTL_CLK_NOC_VPU_CFG_CLK:
            return false;

        /*--------------------------- PMU CLOCK ------------------------------------*/
        case SYSCTL_CLK_PMU_PCLK:
            return false;       /* always osc24m */

        /*--------------------------- HS CLOCK ------------------------------------*/
        case SYSCTL_CLK_NOC_HS_OSPI_HCLK:
        case SYSCTL_CLK_HS_HCLK_HIGH:
        case SYSCTL_CLK_NOC_HS_SUB_HCLK:
        case SYSCTL_CLK_SD0_HCLK:
        case SYSCTL_CLK_SD1_HCLK:
        case SYSCTL_CLK_USB0_HCLK:
        case SYSCTL_CLK_USB1_HCLK:
        case SYSCTL_CLK_SSI0_HCLK:
        case SYSCTL_CLK_SSI1_HCLK:
        case SYSCTL_CLK_SSI2_HCLK:
            return false;       /* always pll0_div4 */

        case SYSCTL_CLK_NOC_HS_OSPI_ACLK:
        case SYSCTL_CLK_SSI1_CLK:
        case SYSCTL_CLK_SSI2_CLK:
        case SYSCTL_CLK_NOC_HS_QSPI_ACLK:
        case SYSCTL_CLK_SSI1_ACLK:
        case SYSCTL_CLK_SSI2_ACLK:
            return false;       /* always pll0_div4 */

        case SYSCTL_CLK_SSI0_CLK:
        {
            if(SYSCTL_CLK_PLL0_OCLK_DIV2 == parent)
            {
                ret = sysctl_clk->hs_spi_cfg;
                ret &= 0xfffbffff;
                sysctl_clk->hs_spi_cfg = ret | (0 << 18);
                return true;
            }
            else if(SYSCTL_CLK_PLL2_OCLK_DIV4 == parent)
            {
                ret = sysctl_clk->hs_spi_cfg;
                ret &= 0xfffbffff;
                sysctl_clk->hs_spi_cfg = ret | (1 << 18);
                return true;
            }
            else
            {
                return false;
            }
        }

        case SYSCTL_CLK_NOC_HS_SD_ACLK:
        case SYSCTL_CLK_SD0_ACLK:
        case SYSCTL_CLK_SD1_ACLK:
        case SYSCTL_CLK_SD_BCLK:
        case SYSCTL_CLK_SD0_BCLK:
        case SYSCTL_CLK_SD1_BCLK:
            return false;

        case SYSCTL_CLK_SD_CCLK:
        case SYSCTL_CLK_SD0_CCLK_TX:
        case SYSCTL_CLK_SD1_CCLK_TX:
            return false;

        case SYSCTL_CLK_PLL0_OCLK_DIV16:
            return false;
        case SYSCTL_CLK_USB_REFCLK_50M:
            return false;
        case SYSCTL_CLK_USB0_REFCLK:
        case SYSCTL_CLK_USB1_REFCLK:
        {
            if(SYSCTL_CLK_OSC24M == parent)
            {
                ret = sysctl_clk->hs_clken_cfg;
                ret &= 0xff7fffff;
                sysctl_clk->hs_clken_cfg = ret | (0 << 23);
                return true;
            }
            else if(SYSCTL_CLK_USB_REFCLK_50M == parent)
            {
                ret = sysctl_clk->hs_clken_cfg;
                ret &= 0xff7fffff;
                sysctl_clk->hs_clken_cfg = ret | (1 << 23);
                return true;
            }
            else
            {
                return false;
            }
        }

        case SYSCTL_CLK_SD_TMCLK:
        case SYSCTL_CLK_SD0_TMCLK:
        case SYSCTL_CLK_SD1_TMCLK:
            return false;

        /*--------------------------- LS CLOCK ------------------------------------*/
        case SYSCTL_CLK_NOC_LS_PCLK:
        case SYSCTL_CLK_UART0_PCLK:
        case SYSCTL_CLK_UART1_PCLK:
        case SYSCTL_CLK_UART2_PCLK:
        case SYSCTL_CLK_UART3_PCLK:
        case SYSCTL_CLK_UART4_PCLK:
        case SYSCTL_CLK_I2C0_PCLK:
        case SYSCTL_CLK_I2C1_PCLK:
        case SYSCTL_CLK_I2C2_PCLK:
        case SYSCTL_CLK_I2C3_PCLK:
        case SYSCTL_CLK_I2C4_PCLK:
        case SYSCTL_CLK_GPIO_PCLK:
        case SYSCTL_CLK_PWM_PCLK:
        case SYSCTL_CLK_JAMLINK0_PCLK:
        case SYSCTL_CLK_JAMLINK1_PCLK:
        case SYSCTL_CLK_JAMLINK2_PCLK:
        case SYSCTL_CLK_JAMLINK3_PCLK:
        case SYSCTL_CLK_ADC_PCLK:
        case SYSCTL_CLK_AUDIO_PCLK:
        case SYSCTL_CLK_CODEC_PCLK:
            return false;

        case SYSCTL_CLK_UART0_CLK:
        case SYSCTL_CLK_UART1_CLK:
        case SYSCTL_CLK_UART2_CLK:
        case SYSCTL_CLK_UART3_CLK:
        case SYSCTL_CLK_UART4_CLK:
            return false;

        case SYSCTL_CLK_JAMLINK0_CO:
        case SYSCTL_CLK_JAMLINK1_CO:
        case SYSCTL_CLK_JAMLINK2_CO:
        case SYSCTL_CLK_JAMLINK3_CO:
            return false;

        case SYSCTL_CLK_I2C0_CLK:
        case SYSCTL_CLK_I2C1_CLK:
        case SYSCTL_CLK_I2C2_CLK:
        case SYSCTL_CLK_I2C3_CLK:
        case SYSCTL_CLK_I2C4_CLK:
            return false;

        case SYSCTL_CLK_ADC_CLK:
            return false;

        case SYSCTL_CLK_GPIO_DBCLK:
            return false;

        /*--------------------------- AUDIO CLOCK ------------------------------------*/
        case SYSCTL_CLK_CODEC_ADC_MCLK:
        case SYSCTL_CLK_CODEC_DAC_MCLK:
        case SYSCTL_CLK_AUD_DEV_CLK:
        case SYSCTL_CLK_PDM_CLK_ROOT:
            return false;

        /*--------------------------- SYSCTL CLOCK ------------------------------------*/
        case SYSCTL_CLK_SYSCTL_PCLK:
        case SYSCTL_CLK_WDT0_PCLK:
        case SYSCTL_CLK_WDT1_PCLK:
        case SYSCTL_CLK_TIMER_PCLK:
        case SYSCTL_CLK_IOMUX_PCLK:
        case SYSCTL_CLK_MAILBOX_PCLK:
            return false;

        case SYSCTL_CLK_HDI_CLK:
            return false;

        case SYSCTL_CLK_STC_CLK:
            return false;

        case SYSCTL_CLK_TS_CLK:
        case SYSCTL_CLK_WDT0_CLK:
        case SYSCTL_CLK_WDT1_CLK:
            return false;

        /*--------------------------- TIMER CLOCK ------------------------------------*/
        case SYSCTL_CLK_TIMERX_PULSE_IN:
            return false;
        case SYSCTL_CLK_TIMER0_CLK:
        {
            if(SYSCTL_CLK_PLL0_OCLK_DIV16 == parent)
            {
                ret = sysctl_clk->sysctl_clken_cfg;
                ret &= 0xffffff7f;
                sysctl_clk->sysctl_clken_cfg = ret | (0 << 7);
                return true;
            }
            else if(SYSCTL_CLK_TIMERX_PULSE_IN == parent)
            {
                ret = sysctl_clk->sysctl_clken_cfg;
                ret &= 0xffffff7f;
                sysctl_clk->sysctl_clken_cfg = ret | (1 << 7);
                return true;
            }
            else
            {
                return false;
            }
        }

        case SYSCTL_CLK_TIMER1_CLK:
        {
            if(SYSCTL_CLK_PLL0_OCLK_DIV16 == parent)
            {
                ret = sysctl_clk->sysctl_clken_cfg;
                ret &= 0xfffffeff;
                sysctl_clk->sysctl_clken_cfg = ret | (0 << 8);
                return true;
            }
            else if(SYSCTL_CLK_TIMERX_PULSE_IN == parent)
            {
                ret = sysctl_clk->sysctl_clken_cfg;
                ret &= 0xfffffeff;
                sysctl_clk->sysctl_clken_cfg = ret | (1 << 8);
                return true;
            }
            else
            {
                return false;
            }
        }

        case SYSCTL_CLK_TIMER2_CLK:
        {
            if(SYSCTL_CLK_PLL0_OCLK_DIV16 == parent)
            {
                ret = sysctl_clk->sysctl_clken_cfg;
                ret &= 0xfffffdff;
                sysctl_clk->sysctl_clken_cfg = ret | (0 << 9);
                return true;
            }
            else if(SYSCTL_CLK_TIMERX_PULSE_IN == parent)
            {
                ret = sysctl_clk->sysctl_clken_cfg;
                ret &= 0xfffffdff;
                sysctl_clk->sysctl_clken_cfg = ret | (1 << 9);
                return true;
            }
            else
            {
                return false;
            }
        }

        case SYSCTL_CLK_TIMER3_CLK:
        {
            if(SYSCTL_CLK_PLL0_OCLK_DIV16 == parent)
            {
                ret = sysctl_clk->sysctl_clken_cfg;
                ret &= 0xfffffbff;
                sysctl_clk->sysctl_clken_cfg = ret | (0 << 10);
                return true;
            }
            else if(SYSCTL_CLK_TIMERX_PULSE_IN == parent)
            {
                ret = sysctl_clk->sysctl_clken_cfg;
                ret &= 0xfffffbff;
                sysctl_clk->sysctl_clken_cfg = ret | (1 << 10);
                return true;
            }
            else
            {
                return false;
            }
        }

        case SYSCTL_CLK_TIMER4_CLK:
        {
            if(SYSCTL_CLK_PLL0_OCLK_DIV16 == parent)
            {
                ret = sysctl_clk->sysctl_clken_cfg;
                ret &= 0xfffff7ff;
                sysctl_clk->sysctl_clken_cfg = ret | (0 << 11);
                return true;
            }
            else if(SYSCTL_CLK_TIMERX_PULSE_IN == parent)
            {
                ret = sysctl_clk->sysctl_clken_cfg;
                ret &= 0xfffff7ff;
                sysctl_clk->sysctl_clken_cfg = ret | (1 << 11);
                return true;
            }
            else
            {
                return false;
            }
        }

        case SYSCTL_CLK_TIMER5_CLK:
        {
            if(SYSCTL_CLK_PLL0_OCLK_DIV16 == parent)
            {
                ret = sysctl_clk->sysctl_clken_cfg;
                ret &= 0xffffefff;
                sysctl_clk->sysctl_clken_cfg = ret | (0 << 12);
                return true;
            }
            else if(SYSCTL_CLK_TIMERX_PULSE_IN == parent)
            {
                ret = sysctl_clk->sysctl_clken_cfg;
                ret &= 0xffffefff;
                sysctl_clk->sysctl_clken_cfg = ret | (1 << 12);
                return true;
            }
            else
            {
                return false;
            }
        }

        /*--------------------------- SHRM CLOCK ------------------------------------*/
        case SYSCTL_CLK_SRAM_CLK:
        {
            if(SYSCTL_CLK_PLL0_OCLK_DIV2 == parent)
            {
                ret = sysctl_clk->shrm_clk_cfg;
                ret &= 0xffffbfff;
                sysctl_clk->shrm_clk_cfg = ret | (0 << 14);
                return true;
            }
            else if(SYSCTL_CLK_PLL3_OCLK_DIV2 == parent)
            {
                ret = sysctl_clk->shrm_clk_cfg;
                ret &= 0xffffbfff;
                sysctl_clk->shrm_clk_cfg = ret | (1 << 14);
                return true;
            }
            else
            {
                return false;
            }
        }
        case SYSCTL_CLK_SHRM_DIV2:
        case SYSCTL_CLK_SHRM_AXIM_CLK:
        case SYSCTL_CLK_SHRM_AXIS_CLK:
        case SYSCTL_CLK_DECOMP_PCLK:
        case SYSCTL_CLK_DECOMP_CLK:
            return false;

        case SYSCTL_CLK_NOC_SHRM_PCLK:
            return false;

        case SYSCTL_CLK_NOC_SHRM_MCLK:
        case SYSCTL_CLK_GSDMA_PCLK:
        case SYSCTL_CLK_GSDMA_CLK:
        case SYSCTL_CLK_NOAI_2D_PCLK:
        case SYSCTL_CLK_NOAI_2D_CLK:
        case SYSCTL_CLK_PDMA_PCLK:
        case SYSCTL_CLK_PDMA_CLK:
            return false;

        /*--------------------------- DDR CLOCK ------------------------------------*/
        case SYSCTL_CLK_DDRC_CORE_CLK:
        {
            uint32_t value;

            if(SYSCTL_CLK_PLL0_OCLK_DIV2 == parent)
                value = 0;
            else if(SYSCTL_CLK_PLL0_OCLK_DIV3 == parent)
                value = 1;
            else if(SYSCTL_CLK_PLL2_OCLK_DIV4 == parent)
                value = 2;
            else
                return false;

            ret = sysctl_clk->ddr_clk_cfg;
            ret &= 0xfffffffc;
            sysctl_clk->ddr_clk_cfg = ret | (value << 0);
            return true;
        }
        case SYSCTL_CLK_DDR_BYPASSPCLK:
        case SYSCTL_CLK_NOC_DDRC_PCLK:
        case SYSCTL_CLK_DDRC_P0_CLK:
        case SYSCTL_CLK_NOC_DDRC_P4_CLK:
        case SYSCTL_CLK_NOC_DDRC_P3_ACLK:
        case SYSCTL_CLK_NOC_DDRC_P2_ACLK:
        case SYSCTL_CLK_NOC_DDRC_P1_ACLK:
            return false;

        /*--------------------------- ISP CLOCK ------------------------------------*/
        case SYSCTL_CLK_ISP_CFG_CLK:
        case SYSCTL_CLK_ISP_PCLK:
        case SYSCTL_CLK_CSI0_CFG_CLK:
        case SYSCTL_CLK_CSI1_CFG_CLK:
        case SYSCTL_CLK_CSI2_CFG_CLK:
        case SYSCTL_CLK_ISP_DPHY_PCLK:
        case SYSCTL_CLK_CSI0_PIXEL_CLK:
        case SYSCTL_CLK_CSI1_PIXEL_CLK:
        case SYSCTL_CLK_CSI2_PIXEL_CLK:
        case SYSCTL_CLK_CSI0_PCLK:
        case SYSCTL_CLK_CSI1_PCLK:
        case SYSCTL_CLK_CSI2_PCLK:
        case SYSCTL_CLK_ISP_DW_HCLK:
        case SYSCTL_CLK_ISP_DW_ACLK:
            return false;

        case SYSCTL_CLK_MCLK0:
        {
            if(SYSCTL_CLK_PLL1_OCLK_DIV3 == parent)
            {
                ret = sysctl_clk->mclk_cfg;
                ret &= 0xffffffe7;
                sysctl_clk->mclk_cfg = ret | (0 << 3);
                return true;
            }
            else if(SYSCTL_CLK_PLL1_OCLK_DIV4 == parent)
            {
                ret = sysctl_clk->mclk_cfg;
                ret &= 0xffffffe7;
                sysctl_clk->mclk_cfg = ret | (1 << 3);
                return true;
            }
            else if(SYSCTL_CLK_PLL0_OCLK_DIV4 == parent)
            {
                ret = sysctl_clk->mclk_cfg;
                ret &= 0xffffffe7;
                sysctl_clk->mclk_cfg = ret | (2 << 3);
                return true;
            }
            else
            {
                return false;
            }
        }
        case SYSCTL_CLK_MCLK1:
        {
            if(SYSCTL_CLK_PLL1_OCLK_DIV3 == parent)
            {
                ret = sysctl_clk->mclk_cfg;
                ret &= 0xfffff3ff;
                sysctl_clk->mclk_cfg = ret | (0 << 10);
                return true;
            }
            else if(SYSCTL_CLK_PLL1_OCLK_DIV4 == parent)
            {
                ret = sysctl_clk->mclk_cfg;
                ret &= 0xfffff3ff;
                sysctl_clk->mclk_cfg = ret | (1 << 10);
                return true;
            }
            else if(SYSCTL_CLK_PLL0_OCLK_DIV4 == parent)
            {
                ret = sysctl_clk->mclk_cfg;
                ret &= 0xfffff3ff;
                sysctl_clk->mclk_cfg = ret | (2 << 10);
                return true;
            }
            else
            {
                return false;
            }
        }
        case SYSCTL_CLK_MCLK2:
        {
            if(SYSCTL_CLK_PLL1_OCLK_DIV3 == parent)
            {
                ret = sysctl_clk->mclk_cfg;
                ret &= 0xfff9ffff;
                sysctl_clk->mclk_cfg = ret | (0 << 17);
                return true;
            }
            else if(SYSCTL_CLK_PLL1_OCLK_DIV4 == parent)
            {
                ret = sysctl_clk->mclk_cfg;
                ret &= 0xfff9ffff;
                sysctl_clk->mclk_cfg = ret | (1 << 17);
                return true;
            }
            else if(SYSCTL_CLK_PLL0_OCLK_DIV4 == parent)
            {
                ret = sysctl_clk->mclk_cfg;
                ret &= 0xfff9ffff;
                sysctl_clk->mclk_cfg = ret | (2 << 17);
                return true;
            }
            else
            {
                return false;
            }
        }

        case SYSCTL_CLK_ISP_CLK:
        {
            if(SYSCTL_CLK_PLL0_OCLK_DIV4 == parent)
            {
                ret = sysctl_clk->isp_clkdiv_cfg;
                ret &= 0xdfffffff;
                sysctl_clk->isp_clkdiv_cfg = ret | (0 << 29);
                return true;
            }
            else if(SYSCTL_CLK_PLL2_OCLK_DIV4 == parent)
            {
                ret = sysctl_clk->isp_clkdiv_cfg;
                ret &= 0xdfffffff;
                sysctl_clk->isp_clkdiv_cfg = ret | (1 << 29);
                return true;
            }
            else
            {
                return false;
            }
        }
        case SYSCTL_CLK_ISP_MCM_CLK:
            return false;

        case SYSCTL_CLK_ISP_HCLK:
            return false;

        case SYSCTL_CLK_ISP_ACLK:
            return false;

        case SYSCTL_CLK_ISPDWE_CLK:
        case SYSCTL_CLK_ISPVSE_CLK:
            return false;

        /*--------------------------- DPU CLOCK ------------------------------------*/
        case SYSCTL_CLK_DPU_CLK:
        case SYSCTL_CLK_DPU_ACLK:
        case SYSCTL_CLK_NOC_DPU_PCLK:
            return false;

        /*--------------------------- DISP CLOCK ------------------------------------*/
        case SYSCTL_CLK_DISP_HCLK:
        case SYSCTL_CLK_DISP_ACLK:
        case SYSCTL_CLK_DISP_CLKEXT:
        case SYSCTL_CLK_CLK_GPU:
        case SYSCTL_CLK_DPIPCLK:
        case SYSCTL_CLK_DISP_CFGCLK:
        case SYSCTL_CLK_DISP_REFCLK:
        case SYSCTL_CLK_DISP_VO_PCLK:
        case SYSCTL_CLK_DISP_DSI_PCLK:
        case SYSCTL_CLK_DISP_DSI_PHY_PCLK:
        case SYSCTL_CLK_DISP_GPU_HCLK:
        case SYSCTL_CLK_DISP_VO_ACLK:
        case SYSCTL_CLK_DISP_GPU_ACLK:
            return false;

        /*--------------------------- SEC CLOCK ------------------------------------*/
        case SYSCTL_CLK_SEC_PCLK:
        case SYSCTL_CLK_FIXCLK:
            return false;

        case SYSCTL_CLK_SEC_ACLK:
            return false;

        /*--------------------------- USB TEST MODE CLOCK ------------------------------------*/
        case SYSCTL_CLK_USB_CLK480:
        case SYSCTL_CLK_USB_CLK100:
            return false;

        /*--------------------------- DPHY DFT MODE CLOCK ------------------------------------*/
        case SYSCTL_CLK_DPHY_TEST_CLK:
            return false;

        /*--------------------------- SPI2AXI CLOCK ------------------------------------*/
        case SYSCTL_CLK_SPI2AXI_ACLK:
            return false;

        default:
            return false;
    }
}/* 获取时钟树上叶子节点的时钟源，即读取MUX对应的寄存器的值；默认返回SYSCTL_CLK_ROOT_MAX。 */
sysctl_clk_node_e sysctl_clk_get_leaf_parent(sysctl_clk_node_e leaf)
{
    switch(sysctl_clk_attribute(leaf))
    {
        case 0:
            return SYSCTL_CLK_ROOT_MAX;
        case 1:
        case 3:
            break;
    }

    switch(leaf)
    {
        /*--------------------------- CPU0 CLOCK ------------------------------------*/
        case SYSCTL_CLK_CPU0_CORE_CLK_ROOT:
            return SYSCTL_CLK_PLL0_OCLK_DIV2;
        case SYSCTL_CLK_CPU0_CORE_CLK:
        case SYSCTL_CLK_CPU0_PLICLK:
        case SYSCTL_CLK_CPU0_ACLK:
            return SYSCTL_CLK_CPU0_CORE_CLK_ROOT;
        case SYSCTL_CLK_TDI_PCLK:
        case SYSCTL_CLK_CPU0_PCLK:
            return SYSCTL_CLK_PLL0_OCLK_DIV4;


        /*--------------------------- CPU1 CLOCK ------------------------------------*/
        case SYSCTL_CLK_CPU1_CORE_CLK_SRC:
        {
            switch((sysctl_clk->cpu1_clk_cfg >> 1) & 0x3)
            {
                case 0:
                    return SYSCTL_CLK_PLL1_OCLK_DIV2;
                case 1:
                    return SYSCTL_CLK_PLL3_OCLK;
                case 2:
                    return SYSCTL_CLK_PLL0_OCLK;
                default:
                    return SYSCTL_CLK_ROOT_MAX;
            }
        }
        case SYSCTL_CLK_CPU1_CORE_CLK_ROOT:
            return SYSCTL_CLK_CPU1_CORE_CLK_SRC;
        case SYSCTL_CLK_CPU1_CORE_CLK:
        case SYSCTL_CLK_CPU1_PLICLK:
        case SYSCTL_CLK_CPU1_ACLK:
            return SYSCTL_CLK_CPU1_CORE_CLK_ROOT;

        case SYSCTL_CLK_CPU1_PCLK:
            return SYSCTL_CLK_PLL0_OCLK_DIV4;

        /*--------------------------- AI CLOCK ------------------------------------*/
        case SYSCTL_CLK_AI_CORE_CLK:
        {
            if(0 == ((sysctl_clk->ai_clk_cfg >> 2) & 0x1))
                return SYSCTL_CLK_PLL1_OCLK_DIV2;
            else
                return SYSCTL_CLK_PLL3_OCLK_DIV2;
        }
        case SYSCTL_CLK_NOC_AI_ACLK:
            return SYSCTL_CLK_PLL0_OCLK_DIV4;
        case SYSCTL_CLK_AI_ACLK:
            return SYSCTL_CLK_NOC_AI_ACLK;

        /*--------------------------- VPU CLOCK ------------------------------------*/
        case SYSCTL_CLK_VPU_CLK_ROOT:
            return SYSCTL_CLK_PLL0_OCLK_DIV2;
        case SYSCTL_CLK_VPU_CLK:
            return SYSCTL_CLK_VPU_CLK_ROOT;
        case SYSCTL_CLK_VPU_ACLK_ROOT:
            return SYSCTL_CLK_VPU_CLK_ROOT;
        case SYSCTL_CLK_NOC_VPU_ACLK:
            return SYSCTL_CLK_VPU_ACLK_ROOT;
        case SYSCTL_CLK_VPU_ACLK:
            return SYSCTL_CLK_NOC_VPU_ACLK;
        case SYSCTL_CLK_NOC_VPU_CFG_CLK:
            return SYSCTL_CLK_PLL0_OCLK_DIV4;
        case SYSCTL_CLK_VPU_CFG_CLK:
            return SYSCTL_CLK_NOC_VPU_CFG_CLK;

        /*--------------------------- PMU CLOCK ------------------------------------*/
        case SYSCTL_CLK_NOC_PMU_PCLK:
            return SYSCTL_CLK_OSC24M;
        case SYSCTL_CLK_PMU_PCLK:
            return SYSCTL_CLK_NOC_PMU_PCLK;

        /*--------------------------- HS CLOCK ------------------------------------*/
        case SYSCTL_CLK_NOC_HS_OSPI_HCLK:
            return SYSCTL_CLK_PLL0_OCLK_DIV4;
        case SYSCTL_CLK_HS_HCLK_HIGH:
            return SYSCTL_CLK_NOC_HS_OSPI_HCLK;
        case SYSCTL_CLK_NOC_HS_SUB_HCLK:
            return SYSCTL_CLK_NOC_HS_OSPI_HCLK;
        case SYSCTL_CLK_HS_HCLK:
            return SYSCTL_CLK_NOC_HS_SUB_HCLK;
        case SYSCTL_CLK_SSI0_HCLK:
            return SYSCTL_CLK_HS_HCLK_HIGH;
        case SYSCTL_CLK_SD0_HCLK:
        case SYSCTL_CLK_SD1_HCLK:
        case SYSCTL_CLK_USB0_HCLK:
        case SYSCTL_CLK_USB1_HCLK:
        case SYSCTL_CLK_SSI1_HCLK:
        case SYSCTL_CLK_SSI2_HCLK:
            return SYSCTL_CLK_HS_HCLK;

        case SYSCTL_CLK_NOC_HS_OSPI_ACLK:
            return SYSCTL_CLK_PLL0_OCLK_DIV4;
        case SYSCTL_CLK_SSI0_ACLK:
            return SYSCTL_CLK_NOC_HS_OSPI_ACLK;
        case SYSCTL_CLK_SSI1_CLK:
        case SYSCTL_CLK_SSI2_CLK:
            return SYSCTL_CLK_PLL0_OCLK_DIV4;
        case SYSCTL_CLK_NOC_HS_QSPI_ACLK:
            return SYSCTL_CLK_PLL0_OCLK_DIV4;
        case SYSCTL_CLK_QSPI_ACLK:
            return SYSCTL_CLK_NOC_HS_QSPI_ACLK;
        case SYSCTL_CLK_SSI1_ACLK:
        case SYSCTL_CLK_SSI2_ACLK:
            return SYSCTL_CLK_QSPI_ACLK;

        case SYSCTL_CLK_SSI0_CLK:
        {
            if(0 == ((sysctl_clk->hs_spi_cfg >> 18) & 0x1))
                return SYSCTL_CLK_PLL0_OCLK_DIV2;
            else
                return SYSCTL_CLK_PLL2_OCLK_DIV4;
        }

        case SYSCTL_CLK_NOC_HS_SD_ACLK:
            return SYSCTL_CLK_PLL2_OCLK_DIV4;
        case SYSCTL_CLK_SD_ACLK:
            return SYSCTL_CLK_NOC_HS_SD_ACLK;
        case SYSCTL_CLK_SD0_ACLK:
        case SYSCTL_CLK_SD1_ACLK:
            return SYSCTL_CLK_SD_ACLK;
        case SYSCTL_CLK_SD_BCLK:
            return SYSCTL_CLK_PLL2_OCLK_DIV4;
        case SYSCTL_CLK_SD0_BCLK:
        case SYSCTL_CLK_SD1_BCLK:
            return SYSCTL_CLK_SD_ACLK;

        case SYSCTL_CLK_SD_CCLK:
            return SYSCTL_CLK_PLL0_OCLK_DIV4;
        case SYSCTL_CLK_SD0_CCLK_TX:
        case SYSCTL_CLK_SD1_CCLK_TX:
            return SYSCTL_CLK_SD_CCLK;

        case SYSCTL_CLK_PLL0_OCLK_DIV16:
            return SYSCTL_CLK_PLL0_OCLK;
        case SYSCTL_CLK_USB_REFCLK_50M:
            return SYSCTL_CLK_PLL0_OCLK_DIV16;
        case SYSCTL_CLK_USB0_REFCLK:
        case SYSCTL_CLK_USB1_REFCLK:
        {
            if(0 == ((sysctl_clk->hs_clken_cfg >> 23) & 0x1))
                return SYSCTL_CLK_OSC24M;
            else
                return SYSCTL_CLK_USB_REFCLK_50M;
        }

        case SYSCTL_CLK_SD_TMCLK:
            return SYSCTL_CLK_OSC24M;
        case SYSCTL_CLK_SD0_TMCLK:
        case SYSCTL_CLK_SD1_TMCLK:
            return SYSCTL_CLK_SD_TMCLK;

        /*--------------------------- LS CLOCK ------------------------------------*/
        case SYSCTL_CLK_NOC_LS_PCLK:
            return SYSCTL_CLK_PLL0_OCLK_DIV4;
        case SYSCTL_CLK_LS_PCLK:
            return SYSCTL_CLK_NOC_LS_PCLK;
        case SYSCTL_CLK_UART0_PCLK:
        case SYSCTL_CLK_UART1_PCLK:
        case SYSCTL_CLK_UART2_PCLK:
        case SYSCTL_CLK_UART3_PCLK:
        case SYSCTL_CLK_UART4_PCLK:
        case SYSCTL_CLK_I2C0_PCLK:
        case SYSCTL_CLK_I2C1_PCLK:
        case SYSCTL_CLK_I2C2_PCLK:
        case SYSCTL_CLK_I2C3_PCLK:
        case SYSCTL_CLK_I2C4_PCLK:
        case SYSCTL_CLK_GPIO_PCLK:
        case SYSCTL_CLK_PWM_PCLK:
        case SYSCTL_CLK_JAMLINK0_PCLK:
        case SYSCTL_CLK_JAMLINK1_PCLK:
        case SYSCTL_CLK_JAMLINK2_PCLK:
        case SYSCTL_CLK_JAMLINK3_PCLK:
        case SYSCTL_CLK_ADC_PCLK:
        case SYSCTL_CLK_AUDIO_PCLK:
        case SYSCTL_CLK_CODEC_PCLK:
            return SYSCTL_CLK_LS_PCLK;

        case SYSCTL_CLK_UART0_CLK:
        case SYSCTL_CLK_UART1_CLK:
        case SYSCTL_CLK_UART2_CLK:
        case SYSCTL_CLK_UART3_CLK:
        case SYSCTL_CLK_UART4_CLK:
            return SYSCTL_CLK_PLL0_OCLK_DIV16;

        case SYSCTL_CLK_JAMLINK0_CO:
        case SYSCTL_CLK_JAMLINK1_CO:
        case SYSCTL_CLK_JAMLINK2_CO:
        case SYSCTL_CLK_JAMLINK3_CO:
            return SYSCTL_CLK_PLL0_OCLK_DIV16;

        case SYSCTL_CLK_I2C0_CLK:
        case SYSCTL_CLK_I2C1_CLK:
        case SYSCTL_CLK_I2C2_CLK:
        case SYSCTL_CLK_I2C3_CLK:
        case SYSCTL_CLK_I2C4_CLK:
            return SYSCTL_CLK_PLL0_OCLK_DIV4;

        case SYSCTL_CLK_ADC_CLK:
            return SYSCTL_CLK_PLL0_OCLK_DIV4;

        case SYSCTL_CLK_GPIO_DBCLK:
            return SYSCTL_CLK_OSC24M;

        /*--------------------------- AUDIO CLOCK ------------------------------------*/

        case SYSCTL_CLK_CODEC_ADC_MCLK:
        case SYSCTL_CLK_CODEC_DAC_MCLK:
        case SYSCTL_CLK_AUD_DEV_CLK:
        case SYSCTL_CLK_PDM_CLK_ROOT:
            return SYSCTL_CLK_PLL0_OCLK_DIV4;

        /*--------------------------- SYSCTL CLOCK ------------------------------------*/
        case SYSCTL_CLK_SYSCTL_PCLK:
            return SYSCTL_CLK_PLL0_OCLK_DIV16;
        case SYSCTL_CLK_WDT0_PCLK:
        case SYSCTL_CLK_WDT1_PCLK:
        case SYSCTL_CLK_TIMER_PCLK:
        case SYSCTL_CLK_IOMUX_PCLK:
        case SYSCTL_CLK_MAILBOX_PCLK:
            return SYSCTL_CLK_SYSCTL_PCLK;

        case SYSCTL_CLK_HDI_CLK:
            return SYSCTL_CLK_PLL0_OCLK_DIV4;

        case SYSCTL_CLK_STC_CLK:
            return SYSCTL_CLK_PLL1_OCLK_DIV4;

        case SYSCTL_CLK_TS_CLK:
        case SYSCTL_CLK_WDT0_CLK:
        case SYSCTL_CLK_WDT1_CLK:
            return SYSCTL_CLK_OSC24M;

        /*--------------------------- TIMER CLOCK ------------------------------------*/
        case SYSCTL_CLK_TIMERX_PULSE_IN:
            return SYSCTL_CLK_ROOT_TIMERX_PULSE_IN;     //注意，此处需要review。因为pulse是外部输入信号，这里如何定义其父时钟？
        case SYSCTL_CLK_TIMER0_CLK:
        {
            if(0 == ((sysctl_clk->sysctl_clken_cfg >> 7) & 0x1))
                return SYSCTL_CLK_PLL0_OCLK_DIV16;
            else
                return SYSCTL_CLK_TIMERX_PULSE_IN;
        }
        case SYSCTL_CLK_TIMER1_CLK:
        {
            if(0 == ((sysctl_clk->sysctl_clken_cfg >> 8) & 0x1))
                return SYSCTL_CLK_PLL0_OCLK_DIV16;
            else
                return SYSCTL_CLK_TIMERX_PULSE_IN;
        }
        case SYSCTL_CLK_TIMER2_CLK:
        {
            if(0 == ((sysctl_clk->sysctl_clken_cfg >> 9) & 0x1))
                return SYSCTL_CLK_PLL0_OCLK_DIV16;
            else
                return SYSCTL_CLK_TIMERX_PULSE_IN;
        }
        case SYSCTL_CLK_TIMER3_CLK:
        {
            if(0 == ((sysctl_clk->sysctl_clken_cfg >> 10) & 0x1))
                return SYSCTL_CLK_PLL0_OCLK_DIV16;
            else
                return SYSCTL_CLK_TIMERX_PULSE_IN;
        }
        case SYSCTL_CLK_TIMER4_CLK:
        {
            if(0 == ((sysctl_clk->sysctl_clken_cfg >> 11) & 0x1))
                return SYSCTL_CLK_PLL0_OCLK_DIV16;
            else
                return SYSCTL_CLK_TIMERX_PULSE_IN;
        }
        case SYSCTL_CLK_TIMER5_CLK:
        {
            if(0 == ((sysctl_clk->sysctl_clken_cfg >> 12) & 0x1))
                return SYSCTL_CLK_PLL0_OCLK_DIV16;
            else
                return SYSCTL_CLK_TIMERX_PULSE_IN;
        }

        /*--------------------------- SHRM CLOCK ------------------------------------*/
        case SYSCTL_CLK_SRAM_CLK:
        {
            if(0 == ((sysctl_clk->shrm_clk_cfg >> 14) & 0x1))
                return SYSCTL_CLK_PLL0_OCLK_DIV2;
            else
                return SYSCTL_CLK_PLL3_OCLK_DIV2;
        }
        case SYSCTL_CLK_SHRM_DIV2:
            return SYSCTL_CLK_SRAM_CLK;
        case SYSCTL_CLK_SHRM_AXIM_CLK:
            return SYSCTL_CLK_NOC_SHRM_MCLK;
        case SYSCTL_CLK_SHRM_AXIS_CLK:
            return SYSCTL_CLK_SHRM_DIV2;
        case SYSCTL_CLK_DECOMP_PCLK:
        case SYSCTL_CLK_GSDMA_PCLK:
        case SYSCTL_CLK_NOAI_2D_PCLK:
        case SYSCTL_CLK_PDMA_PCLK:
            return SYSCTL_CLK_SHRM_PCLK;
        case SYSCTL_CLK_DECOMP_CLK:
            return SYSCTL_CLK_SRAM_CLK;

        case SYSCTL_CLK_NOC_SHRM_PCLK:
            return SYSCTL_CLK_PLL0_OCLK_DIV4;
        case SYSCTL_CLK_SHRM_PCLK:
            return SYSCTL_CLK_NOC_SHRM_PCLK;

        case SYSCTL_CLK_NOC_SHRM_MCLK:
            return SYSCTL_CLK_PLL0_OCLK_DIV4;
        case SYSCTL_CLK_GSDMA_CLK:
        case SYSCTL_CLK_NOAI_2D_CLK:
        case SYSCTL_CLK_PDMA_CLK:
            return SYSCTL_CLK_SHRM_AXIM_CLK;

        /*--------------------------- DDR CLOCK ------------------------------------*/
        case SYSCTL_CLK_DDRC_CORE_CLK:
        {
            switch((sysctl_clk->ddr_clk_cfg >> 0) & 0x3)
            {
                case 0:
                    return SYSCTL_CLK_PLL0_OCLK_DIV2;
                case 1:
                    return SYSCTL_CLK_PLL0_OCLK_DIV3;
                case 2:
                case 3:
                    return SYSCTL_CLK_PLL2_OCLK_DIV4;
                default:
                    return SYSCTL_CLK_ROOT_MAX;
            }
        }
        case SYSCTL_CLK_DDR_BYPASSPCLK:
            return SYSCTL_CLK_PLL2_OCLK_DIV4;
        case SYSCTL_CLK_NOC_DDRC_PCLK:
            return SYSCTL_CLK_PLL0_OCLK_DIV4;
        case SYSCTL_CLK_DDRC_PCLK:
            return SYSCTL_CLK_NOC_DDRC_PCLK;
        case SYSCTL_CLK_NOC_DDRC_P0_CLK:
            return SYSCTL_CLK_CPU1_CORE_CLK_ROOT;
        case SYSCTL_CLK_NOC_DDRC_P4_CLK:
            return SYSCTL_CLK_CPU0_CORE_CLK_ROOT;
        case SYSCTL_CLK_NOC_DDRC_P3_ACLK:
            return SYSCTL_CLK_PLL0_OCLK_DIV4;
        case SYSCTL_CLK_NOC_DDRC_P2_ACLK:
            return SYSCTL_CLK_VPU_ACLK_ROOT;
        case SYSCTL_CLK_NOC_DDRC_P1_ACLK:
            return SYSCTL_CLK_PLL0_OCLK_DIV4;

        /*--------------------------- ISP CLOCK ------------------------------------*/
        case SYSCTL_CLK_ISP_CFG_CLK:
            return SYSCTL_CLK_PLL1_OCLK_DIV4;
        case SYSCTL_CLK_ISP_PCLK:
        case SYSCTL_CLK_CSI0_PCLK:
        case SYSCTL_CLK_CSI1_PCLK:
        case SYSCTL_CLK_CSI2_PCLK:
            return SYSCTL_CLK_ISP_HCLK;
        case SYSCTL_CLK_CSI0_CFG_CLK:
        case SYSCTL_CLK_CSI1_CFG_CLK:
        case SYSCTL_CLK_CSI2_CFG_CLK:
        case SYSCTL_CLK_ISP_DPHY_PCLK:
            return SYSCTL_CLK_ISP_CFG_CLK;
        case SYSCTL_CLK_CSI0_PIXEL_CLK:
        case SYSCTL_CLK_CSI1_PIXEL_CLK:
        case SYSCTL_CLK_CSI2_PIXEL_CLK:
            return SYSCTL_CLK_PLL2_OCLK_DIV4;

        case SYSCTL_CLK_MCLK0:
        {
            switch((sysctl_clk->mclk_cfg >> 3) & 0x3)
            {
                case 0:
                    return SYSCTL_CLK_PLL1_OCLK_DIV3;
                case 1:
                    return SYSCTL_CLK_PLL1_OCLK_DIV4;
                case 2:
                    return SYSCTL_CLK_PLL0_OCLK_DIV4;
                default:
                    return SYSCTL_CLK_ROOT_MAX;
            }
        }
        case SYSCTL_CLK_MCLK1:
        {
            switch((sysctl_clk->mclk_cfg >> 10) & 0x3)
            {
                case 0:
                    return SYSCTL_CLK_PLL1_OCLK_DIV3;
                case 1:
                    return SYSCTL_CLK_PLL1_OCLK_DIV4;
                case 2:
                    return SYSCTL_CLK_PLL0_OCLK_DIV4;
                default:
                    return SYSCTL_CLK_ROOT_MAX;
            }
        }
        case SYSCTL_CLK_MCLK2:
        {
            switch((sysctl_clk->mclk_cfg >> 17) & 0x3)
            {
                case 0:
                    return SYSCTL_CLK_PLL1_OCLK_DIV3;
                case 1:
                    return SYSCTL_CLK_PLL1_OCLK_DIV4;
                case 2:
                    return SYSCTL_CLK_PLL0_OCLK_DIV4;
                default:
                    return SYSCTL_CLK_ROOT_MAX;
            }
        }

        case SYSCTL_CLK_ISP_CLK:
        {
            if(0 == ((sysctl_clk->isp_clkdiv_cfg >> 29) & 0x1))
                return SYSCTL_CLK_PLL0_OCLK_DIV4;
            else
                return SYSCTL_CLK_PLL2_OCLK_DIV4;
        }
        case SYSCTL_CLK_ISP_MCM_CLK:
            return SYSCTL_CLK_ISP_CLK;

        case SYSCTL_CLK_NOC_ISP_HCLK:
            return SYSCTL_CLK_PLL0_OCLK_DIV4;
        case SYSCTL_CLK_ISP_HCLK:
            return SYSCTL_CLK_NOC_ISP_HCLK;

        case SYSCTL_CLK_NOC_ISP_ACLK:
            return SYSCTL_CLK_PLL0_OCLK_DIV4;
        case SYSCTL_CLK_ISP_ACLK:
            return SYSCTL_CLK_NOC_ISP_ACLK;

        case SYSCTL_CLK_ISPDWE_CLK:
        case SYSCTL_CLK_ISPVSE_CLK:
            return SYSCTL_CLK_PLL0_OCLK_DIV3;
        case SYSCTL_CLK_ISP_DW_HCLK:
            return SYSCTL_CLK_ISP_HCLK;
        case SYSCTL_CLK_ISP_DW_ACLK:
            return SYSCTL_CLK_ISP_ACLK;

        /*--------------------------- DPU CLOCK ------------------------------------*/
        case SYSCTL_CLK_DPU_CLK:
            return SYSCTL_CLK_PLL1_OCLK_DIV4;
        case SYSCTL_CLK_NOC_DPU_ACLK:
            return SYSCTL_CLK_PLL0_OCLK_DIV4;
        case SYSCTL_CLK_DPU_ACLK:
            return SYSCTL_CLK_NOC_DPU_ACLK;
        case SYSCTL_CLK_NOC_DPU_PCLK:
            return SYSCTL_CLK_PLL0_OCLK_DIV4;
        case SYSCTL_CLK_DPU_PCLK:
            return SYSCTL_CLK_NOC_DPU_PCLK;

        /*--------------------------- DISP CLOCK ------------------------------------*/
        case SYSCTL_CLK_NOC_DISP_HCLK:
        case SYSCTL_CLK_NOC_DISP_ACLK:
            return SYSCTL_CLK_PLL0_OCLK_DIV4;
        case SYSCTL_CLK_DISP_HCLK:
            return SYSCTL_CLK_NOC_DISP_HCLK;
        case SYSCTL_CLK_DISP_ACLK:
            return SYSCTL_CLK_NOC_DISP_ACLK;
        case SYSCTL_CLK_DISP_CLKEXT:
        case SYSCTL_CLK_CLK_GPU:
            return SYSCTL_CLK_PLL0_OCLK_DIV3;
        case SYSCTL_CLK_DPIPCLK:
        case SYSCTL_CLK_DISP_CFGCLK:
            return SYSCTL_CLK_PLL1_OCLK_DIV4;
        case SYSCTL_CLK_DISP_REFCLK:
            return SYSCTL_CLK_OSC24M;
        case SYSCTL_CLK_DISP_VO_PCLK:
        case SYSCTL_CLK_DISP_DSI_PCLK:
        case SYSCTL_CLK_DISP_DSI_PHY_PCLK:
            return SYSCTL_CLK_PLL1_OCLK_DIV4;
        case SYSCTL_CLK_DISP_GPU_HCLK:
            return SYSCTL_CLK_CLK_GPU;
        case SYSCTL_CLK_DISP_VO_ACLK:
            return SYSCTL_CLK_DISP_ACLK;
        case SYSCTL_CLK_DISP_GPU_ACLK:
            return SYSCTL_CLK_CLK_GPU;

        /*--------------------------- SEC CLOCK ------------------------------------*/
        case SYSCTL_CLK_NOC_SEC_PCLK:
            return SYSCTL_CLK_PLL0_OCLK_DIV4;
        case SYSCTL_CLK_SEC_PCLK:
            return SYSCTL_CLK_NOC_SEC_PCLK;

        case SYSCTL_CLK_FIXCLK:
            return SYSCTL_CLK_PLL1_OCLK_DIV4;
        case SYSCTL_CLK_NOC_SEC_ACLK:
            return SYSCTL_CLK_PLL1_OCLK_DIV4;
        case SYSCTL_CLK_SEC_ACLK:
            return SYSCTL_CLK_NOC_SEC_ACLK;

        /*--------------------------- USB TEST MODE CLOCK ------------------------------------*/
        case SYSCTL_CLK_USB_TEST_CLK_GATE:
            return SYSCTL_CLK_ROOT_MAX;
        case SYSCTL_CLK_USB_CLK480:
            return SYSCTL_CLK_PLL1_OCLK;
        case SYSCTL_CLK_USB_CLK100:
            return SYSCTL_CLK_PLL0_OCLK_DIV4;

        /*--------------------------- DPHY DFT MODE CLOCK ------------------------------------*/
        case SYSCTL_CLK_DPHY_TEST_CLK:
            return SYSCTL_CLK_PLL0_OCLK;

        /*--------------------------- SPI2AXI CLOCK ------------------------------------*/
        case SYSCTL_CLK_NOC_SPI2AXI_ACLK:
            return SYSCTL_CLK_PLL0_OCLK_DIV4;
        case SYSCTL_CLK_SPI2AXI_ACLK:
            return SYSCTL_CLK_NOC_SPI2AXI_ACLK;

        /*--------------------------- CLOCK-TREE TOPOLOGY ---------------------------*/
        case SYSCTL_CLK_NOC_CPU1_CLK:
            return SYSCTL_CLK_CPU1_CORE_CLK_ROOT;
        case SYSCTL_CLK_DDRC_P0_CLK:
            return SYSCTL_CLK_NOC_DDRC_P0_CLK;

        case SYSCTL_CLK_NOC_CPU0_CLK:
            return SYSCTL_CLK_CPU0_CORE_CLK_ROOT;
        case SYSCTL_CLK_DDRC_P4_CLK:
            return SYSCTL_CLK_NOC_DDRC_P4_CLK;

        case SYSCTL_CLK_DDRC_P3_CLK:
            return SYSCTL_CLK_NOC_DDRC_P3_ACLK;

        case SYSCTL_CLK_DDRC_P2_CLK:
            return SYSCTL_CLK_NOC_DDRC_P2_ACLK;
        case SYSCTL_CLK_DDRC_P1_CLK:
            return SYSCTL_CLK_NOC_DDRC_P1_ACLK;
        default:
            return SYSCTL_CLK_ROOT_MAX;
    }
}

/* 设置时钟节点enable,注意:只设置本时钟节点的enable，不会设置上游时钟的enable。*/
void sysctl_clk_set_leaf_en(sysctl_clk_node_e leaf, bool enable)
{
    volatile uint32_t ret;
    switch(sysctl_clk_attribute(leaf))
    {
        case 0:
        case 1:
            return;
        case 3:
            break;
    }

    switch(leaf)
    {
        /*--------------------------- CPU0 CLOCK ------------------------------------*/
        case SYSCTL_CLK_CPU0_CORE_CLK:
            if(enable == true)
                sysctl_clk->cpu0_clk_cfg |= (1 << 0);
            else
                sysctl_clk->cpu0_clk_cfg &= ~(1 << 0);
            break;
        case SYSCTL_CLK_CPU0_PLICLK:
            if(enable == true)
                sysctl_clk->cpu0_clk_cfg |= (1 << 9);
            else
                sysctl_clk->cpu0_clk_cfg &= ~(1 << 9);
            break;
        case SYSCTL_CLK_CPU0_ACLK:
            break;

        case SYSCTL_CLK_TDI_PCLK:
            if(enable == true)
                sysctl_clk->cpu0_clk_cfg |= (1 << 14);
            else
                sysctl_clk->cpu0_clk_cfg &= ~(1 << 14);
            break;

        case SYSCTL_CLK_CPU0_PCLK:
            if(enable == true)
                sysctl_clk->cpu0_clk_cfg |= (1 << 13);
            else
                sysctl_clk->cpu0_clk_cfg &= ~(1 << 13);
            break;


        /*--------------------------- CPU1 CLOCK ------------------------------------*/
        case SYSCTL_CLK_CPU1_CORE_CLK:
            if(enable == true)
                sysctl_clk->cpu1_clk_cfg |= (1 << 0);
            else
                sysctl_clk->cpu1_clk_cfg &= ~(1 << 0);
            break;

        case SYSCTL_CLK_CPU1_PLICLK:
            if(enable == true)
                sysctl_clk->cpu1_clk_cfg |= (1 << 15);
            else
                sysctl_clk->cpu1_clk_cfg &= ~(1 << 15);
            break;

        case SYSCTL_CLK_CPU1_ACLK:
            break;



        case SYSCTL_CLK_CPU1_PCLK:
            if(enable == true)
                sysctl_clk->cpu1_clk_cfg |= (1 << 19);
            else
                sysctl_clk->cpu1_clk_cfg &= ~(1 << 19);
            break;

        /*--------------------------- AI CLOCK ------------------------------------*/
        case SYSCTL_CLK_AI_CORE_CLK:
            if(enable == true)
                sysctl_clk->ai_clk_cfg |= (1 << 0);
            else
                sysctl_clk->ai_clk_cfg &= ~(1 << 0);
            break;

        case SYSCTL_CLK_AI_ACLK:
            if(enable == true)
                sysctl_clk->ai_clk_cfg |= (1 << 10);
            else
                sysctl_clk->ai_clk_cfg &= ~(1 << 10);
            break;


        /*--------------------------- VPU CLOCK ------------------------------------*/
        case SYSCTL_CLK_VPU_CLK:
            if(enable == true)
                sysctl_clk->vpu_clk_cfg |= (1 << 0);
            else
                sysctl_clk->vpu_clk_cfg &= ~(1 << 0);
            break;

        case SYSCTL_CLK_VPU_ACLK_ROOT:
            break;

        case SYSCTL_CLK_VPU_ACLK:
            if(enable == true)
                sysctl_clk->vpu_clk_cfg |= (1 << 5);
            else
                sysctl_clk->vpu_clk_cfg &= ~(1 << 5);
            break;


        case SYSCTL_CLK_VPU_CFG_CLK:
            if(enable == true)
                sysctl_clk->vpu_clk_cfg |= (1 << 10);
            else
                sysctl_clk->vpu_clk_cfg &= ~(1 << 10);
            break;

        /*--------------------------- PMU CLOCK ------------------------------------*/
        case SYSCTL_CLK_PMU_PCLK:
            if(enable == true)
                sysctl_clk->pmu_clk_cfg |= (1 << 0);
            else
                sysctl_clk->pmu_clk_cfg &= ~(1 << 0);
            break;

        /*--------------------------- HS CLOCK ------------------------------------*/
        case SYSCTL_CLK_NOC_HS_OSPI_HCLK:
            break;
        case SYSCTL_CLK_HS_HCLK_HIGH:
            if(enable == true)
                sysctl_clk->hs_clken_cfg |= (1 << 1);
            else
                sysctl_clk->hs_clken_cfg &= ~(1 << 1);
            break;

        case SYSCTL_CLK_HS_HCLK:
            if(enable == true)
                sysctl_clk->hs_clken_cfg |= (1 << 0);
            else
                sysctl_clk->hs_clken_cfg &= ~(1 << 0);
            break;

        case SYSCTL_CLK_SD0_HCLK:
            if(enable == true)
                sysctl_clk->hs_clken_cfg |= (1 << 2);
            else
                sysctl_clk->hs_clken_cfg &= ~(1 << 2);
            break;

        case SYSCTL_CLK_SD1_HCLK:
            if(enable == true)
                sysctl_clk->hs_clken_cfg |= (1 << 3);
            else
                sysctl_clk->hs_clken_cfg &= ~(1 << 3);
            break;

        case SYSCTL_CLK_USB0_HCLK:
            if(enable == true)
                sysctl_clk->hs_clken_cfg |= (1 << 4);
            else
                sysctl_clk->hs_clken_cfg &= ~(1 << 4);
            break;

        case SYSCTL_CLK_USB1_HCLK:
            if(enable == true)
                sysctl_clk->hs_clken_cfg |= (1 << 5);
            else
                sysctl_clk->hs_clken_cfg &= ~(1 << 5);
            break;

        case SYSCTL_CLK_SSI0_HCLK:
            if(enable == true)
                sysctl_clk->hs_clken_cfg |= (1 << 6);
            else
                sysctl_clk->hs_clken_cfg &= ~(1 << 6);
            break;

        case SYSCTL_CLK_SSI1_HCLK:
            if(enable == true)
                sysctl_clk->hs_clken_cfg |= (1 << 7);
            else
                sysctl_clk->hs_clken_cfg &= ~(1 << 7);
            break;

        case SYSCTL_CLK_SSI2_HCLK:
            if(enable == true)
                sysctl_clk->hs_clken_cfg |= (1 << 8);
            else
                sysctl_clk->hs_clken_cfg &= ~(1 << 8);
            break;

        case SYSCTL_CLK_SSI0_ACLK:
            if(enable == true)
                sysctl_clk->hs_clken_cfg |= (1 << 27);
            else
                sysctl_clk->hs_clken_cfg &= ~(1 << 27);
            break;

        case SYSCTL_CLK_SSI1_CLK:
            if(enable == true)
                sysctl_clk->hs_clken_cfg |= (1 << 25);
            else
                sysctl_clk->hs_clken_cfg &= ~(1 << 25);
            break;

        case SYSCTL_CLK_SSI2_CLK:
            if(enable == true)
                sysctl_clk->hs_clken_cfg |= (1 << 26);
            else
                sysctl_clk->hs_clken_cfg &= ~(1 << 26);
            break;

        case SYSCTL_CLK_QSPI_ACLK:
            if(enable == true)
                sysctl_clk->hs_clken_cfg |= (1 << 28);
            else
                sysctl_clk->hs_clken_cfg &= ~(1 << 28);
            break;

        case SYSCTL_CLK_SSI1_ACLK:
            if(enable == true)
                sysctl_clk->hs_clken_cfg |= (1 << 29);
            else
                sysctl_clk->hs_clken_cfg &= ~(1 << 29);
            break;

        case SYSCTL_CLK_SSI2_ACLK:
            if(enable == true)
                sysctl_clk->hs_clken_cfg |= (1 << 30);
            else
                sysctl_clk->hs_clken_cfg &= ~(1 << 30);
            break;

        case SYSCTL_CLK_SSI0_CLK:
            if(enable == true)
                sysctl_clk->hs_clken_cfg |= (1 << 24);
            else
                sysctl_clk->hs_clken_cfg &= ~(1 << 24);
            break;

        case SYSCTL_CLK_SD_ACLK:
            if(enable == true)
                sysctl_clk->hs_clken_cfg |= (1 << 9);
            else
                sysctl_clk->hs_clken_cfg &= ~(1 << 9);
            break;

        case SYSCTL_CLK_SD0_ACLK:
            if(enable == true)
                sysctl_clk->hs_clken_cfg |= (1 << 13);
            else
                sysctl_clk->hs_clken_cfg &= ~(1 << 13);
            break;

        case SYSCTL_CLK_SD1_ACLK:
            if(enable == true)
                sysctl_clk->hs_clken_cfg |= (1 << 17);
            else
                sysctl_clk->hs_clken_cfg &= ~(1 << 17);
            break;

        case SYSCTL_CLK_SD_BCLK:
            if(enable == true)
                sysctl_clk->hs_clken_cfg |= (1 << 10);
            else
                sysctl_clk->hs_clken_cfg &= ~(1 << 10);
            break;

        case SYSCTL_CLK_SD0_BCLK:
            if(enable == true)
                sysctl_clk->hs_clken_cfg |= (1 << 14);
            else
                sysctl_clk->hs_clken_cfg &= ~(1 << 14);
            break;

        case SYSCTL_CLK_SD1_BCLK:
            if(enable == true)
                sysctl_clk->hs_clken_cfg |= (1 << 18);
            else
                sysctl_clk->hs_clken_cfg &= ~(1 << 18);
            break;

        case SYSCTL_CLK_SD_CCLK:
            if(enable == true)
                sysctl_clk->hs_clken_cfg |= (1 << 11);
            else
                sysctl_clk->hs_clken_cfg &= ~(1 << 11);
            break;

        case SYSCTL_CLK_SD0_CCLK_TX:
            if(enable == true)
                sysctl_clk->hs_clken_cfg |= (1 << 15);
            else
                sysctl_clk->hs_clken_cfg &= ~(1 << 15);
            break;

        case SYSCTL_CLK_SD1_CCLK_TX:
            if(enable == true)
                sysctl_clk->hs_clken_cfg |= (1 << 19);
            else
                sysctl_clk->hs_clken_cfg &= ~(1 << 19);
            break;

        case SYSCTL_CLK_PLL0_OCLK_DIV16:
            break;
        case SYSCTL_CLK_USB_REFCLK_50M:
            break;

        case SYSCTL_CLK_USB0_REFCLK:
            if(enable == true)
                sysctl_clk->hs_clken_cfg |= (1 << 21);
            else
                sysctl_clk->hs_clken_cfg &= ~(1 << 21);
            break;

        case SYSCTL_CLK_USB1_REFCLK:
            if(enable == true)
                sysctl_clk->hs_clken_cfg |= (1 << 22);
            else
                sysctl_clk->hs_clken_cfg &= ~(1 << 22);
            break;

        case SYSCTL_CLK_SD_TMCLK:
            if(enable == true)
                sysctl_clk->hs_clken_cfg |= (1 << 12);
            else
                sysctl_clk->hs_clken_cfg &= ~(1 << 12);
            break;

        case SYSCTL_CLK_SD0_TMCLK:
            if(enable == true)
                sysctl_clk->hs_clken_cfg |= (1 << 16);
            else
                sysctl_clk->hs_clken_cfg &= ~(1 << 16);
            break;

        case SYSCTL_CLK_SD1_TMCLK:
            if(enable == true)
                sysctl_clk->hs_clken_cfg |= (1 << 20);
            else
                sysctl_clk->hs_clken_cfg &= ~(1 << 20);
            break;

        /*--------------------------- LS CLOCK ------------------------------------*/
        case SYSCTL_CLK_LS_PCLK:
            if(enable == true)
                sysctl_clk->ls_clken_cfg0 |= (1 << 0);
            else
                sysctl_clk->ls_clken_cfg0 &= ~(1 << 0);
            break;

        case SYSCTL_CLK_UART0_PCLK:
            if(enable == true)
                sysctl_clk->ls_clken_cfg0 |= (1 << 1);
            else
                sysctl_clk->ls_clken_cfg0 &= ~(1 << 1);
            break;

        case SYSCTL_CLK_UART1_PCLK:
            if(enable == true)
                sysctl_clk->ls_clken_cfg0 |= (1 << 2);
            else
                sysctl_clk->ls_clken_cfg0 &= ~(1 << 2);
            break;

        case SYSCTL_CLK_UART2_PCLK:
            if(enable == true)
                sysctl_clk->ls_clken_cfg0 |= (1 << 3);
            else
                sysctl_clk->ls_clken_cfg0 &= ~(1 << 3);
            break;

        case SYSCTL_CLK_UART3_PCLK:
            if(enable == true)
                sysctl_clk->ls_clken_cfg0 |= (1 << 4);
            else
                sysctl_clk->ls_clken_cfg0 &= ~(1 << 4);
            break;

        case SYSCTL_CLK_UART4_PCLK:
            if(enable == true)
                sysctl_clk->ls_clken_cfg0 |= (1 << 5);
            else
                sysctl_clk->ls_clken_cfg0 &= ~(1 << 5);
            break;

        case SYSCTL_CLK_I2C0_PCLK:
            if(enable == true)
                sysctl_clk->ls_clken_cfg0 |= (1 << 6);
            else
                sysctl_clk->ls_clken_cfg0 &= ~(1 << 6);
            break;

        case SYSCTL_CLK_I2C1_PCLK:
            if(enable == true)
                sysctl_clk->ls_clken_cfg0 |= (1 << 7);
            else
                sysctl_clk->ls_clken_cfg0 &= ~(1 << 7);
            break;

        case SYSCTL_CLK_I2C2_PCLK:
            if(enable == true)
                sysctl_clk->ls_clken_cfg0 |= (1 << 8);
            else
                sysctl_clk->ls_clken_cfg0 &= ~(1 << 8);
            break;

        case SYSCTL_CLK_I2C3_PCLK:
            if(enable == true)
                sysctl_clk->ls_clken_cfg0 |= (1 << 9);
            else
                sysctl_clk->ls_clken_cfg0 &= ~(1 << 9);
            break;

        case SYSCTL_CLK_I2C4_PCLK:
            if(enable == true)
                sysctl_clk->ls_clken_cfg0 |= (1 << 10);
            else
                sysctl_clk->ls_clken_cfg0 &= ~(1 << 10);
            break;

        case SYSCTL_CLK_GPIO_PCLK:
            if(enable == true)
                sysctl_clk->ls_clken_cfg0 |= (1 << 11);
            else
                sysctl_clk->ls_clken_cfg0 &= ~(1 << 11);
            break;

        case SYSCTL_CLK_PWM_PCLK:
            if(enable == true)
                sysctl_clk->ls_clken_cfg0 |= (1 << 12);
            else
                sysctl_clk->ls_clken_cfg0 &= ~(1 << 12);
            break;

        case SYSCTL_CLK_JAMLINK0_PCLK:
            if(enable == true)
                sysctl_clk->ls_clken_cfg1 |= (1 << 4);
            else
                sysctl_clk->ls_clken_cfg1 &= ~(1 << 4);
            break;

        case SYSCTL_CLK_JAMLINK1_PCLK:
            if(enable == true)
                sysctl_clk->ls_clken_cfg1 |= (1 << 5);
            else
                sysctl_clk->ls_clken_cfg1 &= ~(1 << 5);
            break;

        case SYSCTL_CLK_JAMLINK2_PCLK:
            if(enable == true)
                sysctl_clk->ls_clken_cfg1 |= (1 << 6);
            else
                sysctl_clk->ls_clken_cfg1 &= ~(1 << 6);
            break;

        case SYSCTL_CLK_JAMLINK3_PCLK:
            if(enable == true)
                sysctl_clk->ls_clken_cfg1 |= (1 << 7);
            else
                sysctl_clk->ls_clken_cfg1 &= ~(1 << 7);
            break;

        case SYSCTL_CLK_ADC_PCLK:
            if(enable == true)
                sysctl_clk->ls_clken_cfg0 |= (1 << 15);
            else
                sysctl_clk->ls_clken_cfg0 &= ~(1 << 15);
            break;

        case SYSCTL_CLK_UART0_CLK:
            if(enable == true)
                sysctl_clk->ls_clken_cfg0 |= (1 << 16);
            else
                sysctl_clk->ls_clken_cfg0 &= ~(1 << 16);
            break;

        case SYSCTL_CLK_UART1_CLK:
            if(enable == true)
                sysctl_clk->ls_clken_cfg0 |= (1 << 17);
            else
                sysctl_clk->ls_clken_cfg0 &= ~(1 << 17);
            break;

        case SYSCTL_CLK_UART2_CLK:
            if(enable == true)
                sysctl_clk->ls_clken_cfg0 |= (1 << 18);
            else
                sysctl_clk->ls_clken_cfg0 &= ~(1 << 18);
            break;

        case SYSCTL_CLK_UART3_CLK:
            if(enable == true)
                sysctl_clk->ls_clken_cfg0 |= (1 << 19);
            else
                sysctl_clk->ls_clken_cfg0 &= ~(1 << 19);
            break;

        case SYSCTL_CLK_UART4_CLK:
            if(enable == true)
                sysctl_clk->ls_clken_cfg0 |= (1 << 20);
            else
                sysctl_clk->ls_clken_cfg0 &= ~(1 << 20);
            break;

        case SYSCTL_CLK_JAMLINK0_CO:
            if(enable == true)
                sysctl_clk->ls_clken_cfg1 |= (1 << 0);
            else
                sysctl_clk->ls_clken_cfg1 &= ~(1 << 0);
            break;

        case SYSCTL_CLK_JAMLINK1_CO:
            if(enable == true)
                sysctl_clk->ls_clken_cfg1 |= (1 << 1);
            else
                sysctl_clk->ls_clken_cfg1 &= ~(1 << 1);
            break;

        case SYSCTL_CLK_JAMLINK2_CO:
            if(enable == true)
                sysctl_clk->ls_clken_cfg1 |= (1 << 2);
            else
                sysctl_clk->ls_clken_cfg1 &= ~(1 << 2);
            break;

        case SYSCTL_CLK_JAMLINK3_CO:
            if(enable == true)
                sysctl_clk->ls_clken_cfg1 |= (1 << 3);
            else
                sysctl_clk->ls_clken_cfg1 &= ~(1 << 3);
            break;

        case SYSCTL_CLK_I2C0_CLK:
            if(enable == true)
                sysctl_clk->ls_clken_cfg0 |= (1 << 21);
            else
                sysctl_clk->ls_clken_cfg0 &= ~(1 << 21);
            break;

        case SYSCTL_CLK_I2C1_CLK:
            if(enable == true)
                sysctl_clk->ls_clken_cfg0 |= (1 << 22);
            else
                sysctl_clk->ls_clken_cfg0 &= ~(1 << 22);
            break;

        case SYSCTL_CLK_I2C2_CLK:
            if(enable == true)
                sysctl_clk->ls_clken_cfg0 |= (1 << 23);
            else
                sysctl_clk->ls_clken_cfg0 &= ~(1 << 23);
            break;

        case SYSCTL_CLK_I2C3_CLK:
            if(enable == true)
                sysctl_clk->ls_clken_cfg0 |= (1 << 24);
            else
                sysctl_clk->ls_clken_cfg0 &= ~(1 << 24);
            break;

        case SYSCTL_CLK_I2C4_CLK:
            if(enable == true)
                sysctl_clk->ls_clken_cfg0 |= (1 << 25);
            else
                sysctl_clk->ls_clken_cfg0 &= ~(1 << 25);
            break;

        case SYSCTL_CLK_ADC_CLK:
            if(enable == true)
                sysctl_clk->ls_clken_cfg0 |= (1 << 26);
            else
                sysctl_clk->ls_clken_cfg0 &= ~(1 << 26);
            break;

        case SYSCTL_CLK_GPIO_DBCLK:
            if(enable == true)
                sysctl_clk->ls_clken_cfg0 |= (1 << 27);
            else
                sysctl_clk->ls_clken_cfg0 &= ~(1 << 27);
            break;

        case SYSCTL_CLK_AUDIO_PCLK:
            if(enable == true)
                sysctl_clk->ls_clken_cfg0 |= (1 << 13);
            else
                sysctl_clk->ls_clken_cfg0 &= ~(1 << 13);
            break;

        case SYSCTL_CLK_CODEC_PCLK:
            if(enable == true)
                sysctl_clk->ls_clken_cfg0 |= (1 << 14);
            else
                sysctl_clk->ls_clken_cfg0 &= ~(1 << 14);
            break;

        /*--------------------------- AUDIO CLOCK ------------------------------------*/


        case SYSCTL_CLK_CODEC_ADC_MCLK:
            if(enable == true)
                sysctl_clk->ls_clken_cfg0 |= (1 << 29);
            else
                sysctl_clk->ls_clken_cfg0 &= ~(1 << 29);
            break;

        case SYSCTL_CLK_CODEC_DAC_MCLK:
            if(enable == true)
                sysctl_clk->ls_clken_cfg0 |= (1 << 30);
            else
                sysctl_clk->ls_clken_cfg0 &= ~(1 << 30);
            break;

        case SYSCTL_CLK_AUD_DEV_CLK:
            if(enable == true)
                sysctl_clk->ls_clken_cfg0 |= (1 << 28);
            else
                sysctl_clk->ls_clken_cfg0 &= ~(1 << 28);
            break;

        case SYSCTL_CLK_PDM_CLK_ROOT:
            if(enable == true)
                sysctl_clk->ls_clken_cfg0 |= (1 << 31);
            else
                sysctl_clk->ls_clken_cfg0 &= ~(1 << 31);
            break;

        /*--------------------------- SYSCTL CLOCK ------------------------------------*/
        case SYSCTL_CLK_SYSCTL_PCLK:
            break;
        case SYSCTL_CLK_WDT0_PCLK:
            if(enable == true)
                sysctl_clk->sysctl_clken_cfg |= (1 << 1);
            else
                sysctl_clk->sysctl_clken_cfg &= ~(1 << 1);
            break;

        case SYSCTL_CLK_WDT1_PCLK:
            if(enable == true)
                sysctl_clk->sysctl_clken_cfg |= (1 << 2);
            else
                sysctl_clk->sysctl_clken_cfg &= ~(1 << 2);
            break;

        case SYSCTL_CLK_TIMER_PCLK:
            if(enable == true)
                sysctl_clk->sysctl_clken_cfg |= (1 << 3);
            else
                sysctl_clk->sysctl_clken_cfg &= ~(1 << 3);
            break;

        case SYSCTL_CLK_IOMUX_PCLK:
            if(enable == true)
                sysctl_clk->sysctl_clken_cfg |= (1 << 20);
            else
                sysctl_clk->sysctl_clken_cfg &= ~(1 << 20);
            break;

        case SYSCTL_CLK_MAILBOX_PCLK:
            if(enable == true)
                sysctl_clk->sysctl_clken_cfg |= (1 << 4);
            else
                sysctl_clk->sysctl_clken_cfg &= ~(1 << 4);
            break;

        case SYSCTL_CLK_HDI_CLK:
            if(enable == true)
                sysctl_clk->sysctl_clken_cfg |= (1 << 21);
            else
                sysctl_clk->sysctl_clken_cfg &= ~(1 << 21);
            break;

        case SYSCTL_CLK_STC_CLK:
            if(enable == true)
                sysctl_clk->sysctl_clken_cfg |= (1 << 19);
            else
                sysctl_clk->sysctl_clken_cfg &= ~(1 << 19);
            break;

        case SYSCTL_CLK_TS_CLK:
            break;

        case SYSCTL_CLK_WDT0_CLK:
            if(enable == true)
                sysctl_clk->sysctl_clken_cfg |= (1 << 5);
            else
                sysctl_clk->sysctl_clken_cfg &= ~(1 << 5);
            break;

        case SYSCTL_CLK_WDT1_CLK:
            if(enable == true)
                sysctl_clk->sysctl_clken_cfg |= (1 << 6);
            else
                sysctl_clk->sysctl_clken_cfg &= ~(1 << 6);
            break;

        /*--------------------------- TIMER CLOCK ------------------------------------*/
        case SYSCTL_CLK_TIMERX_PULSE_IN:
            break;
        case SYSCTL_CLK_TIMER0_CLK:
            if(enable == true)
                sysctl_clk->sysctl_clken_cfg |= (1 << 13);
            else
                sysctl_clk->sysctl_clken_cfg &= ~(1 << 13);
            break;

        case SYSCTL_CLK_TIMER1_CLK:
            if(enable == true)
                sysctl_clk->sysctl_clken_cfg |= (1 << 14);
            else
                sysctl_clk->sysctl_clken_cfg &= ~(1 << 14);
            break;

        case SYSCTL_CLK_TIMER2_CLK:
            if(enable == true)
                sysctl_clk->sysctl_clken_cfg |= (1 << 15);
            else
                sysctl_clk->sysctl_clken_cfg &= ~(1 << 15);
            break;

        case SYSCTL_CLK_TIMER3_CLK:
            if(enable == true)
                sysctl_clk->sysctl_clken_cfg |= (1 << 16);
            else
                sysctl_clk->sysctl_clken_cfg &= ~(1 << 16);
            break;

        case SYSCTL_CLK_TIMER4_CLK:
            if(enable == true)
                sysctl_clk->sysctl_clken_cfg |= (1 << 17);
            else
                sysctl_clk->sysctl_clken_cfg &= ~(1 << 17);
            break;

        case SYSCTL_CLK_TIMER5_CLK:
            if(enable == true)
                sysctl_clk->sysctl_clken_cfg |= (1 << 18);
            else
                sysctl_clk->sysctl_clken_cfg &= ~(1 << 18);
            break;

        /*--------------------------- SHRM CLOCK ------------------------------------*/
        case SYSCTL_CLK_SRAM_CLK:
            if(enable == true)
                sysctl_clk->shrm_clk_cfg |= (1 << 10);
            else
                sysctl_clk->shrm_clk_cfg &= ~(1 << 10);
            break;

        case SYSCTL_CLK_SHRM_DIV2:
            break;
        case SYSCTL_CLK_DECOMP_PCLK:
            if(enable == true)
                sysctl_clk->shrm_clk_cfg |= (1 << 6);
            else
                sysctl_clk->shrm_clk_cfg &= ~(1 << 6);
            break;
        case SYSCTL_CLK_SHRM_AXIM_CLK:
            if(enable == true)
                sysctl_clk->shrm_clk_cfg |= (1 << 13);
            else
                sysctl_clk->shrm_clk_cfg &= ~(1 << 13);
            break;
        case SYSCTL_CLK_SHRM_AXIS_CLK:
            if(enable == true)
                sysctl_clk->shrm_clk_cfg |= (1 << 11);
            else
                sysctl_clk->shrm_clk_cfg &= ~(1 << 11);
            break;

        case SYSCTL_CLK_DECOMP_CLK:
            if(enable == true)
                sysctl_clk->shrm_clk_cfg |= (1 << 7);
            else
                sysctl_clk->shrm_clk_cfg &= ~(1 << 7);
            break;

        case SYSCTL_CLK_SHRM_PCLK:
            if(enable == true)
                sysctl_clk->shrm_clk_cfg |= (1 << 0);
            else
                sysctl_clk->shrm_clk_cfg &= ~(1 << 0);
            break;

        case SYSCTL_CLK_NOC_SHRM_MCLK:
            if(enable == true)
                sysctl_clk->shrm_clk_cfg |= (1 << 12);
            else
                sysctl_clk->shrm_clk_cfg &= ~(1 << 12);
            break;

        case SYSCTL_CLK_GSDMA_PCLK:
            if(enable == true)
                sysctl_clk->shrm_clk_cfg |= (1 << 4);
            else
                sysctl_clk->shrm_clk_cfg &= ~(1 << 4);
            break;
        case SYSCTL_CLK_GSDMA_CLK:
            if(enable == true)
                sysctl_clk->shrm_clk_cfg |= (1 << 5);
            else
                sysctl_clk->shrm_clk_cfg &= ~(1 << 5);
            break;

        case SYSCTL_CLK_NOAI_2D_PCLK:
            if(enable == true)
                sysctl_clk->shrm_clk_cfg |= (1 << 8);
            else
                sysctl_clk->shrm_clk_cfg &= ~(1 << 8);
            break;
        case SYSCTL_CLK_NOAI_2D_CLK:
            if(enable == true)
                sysctl_clk->shrm_clk_cfg |= (1 << 9);
            else
                sysctl_clk->shrm_clk_cfg &= ~(1 << 9);
            break;

        case SYSCTL_CLK_PDMA_PCLK:
            if(enable == true)
                sysctl_clk->shrm_clk_cfg |= (1 << 2);
            else
                sysctl_clk->shrm_clk_cfg &= ~(1 << 2);
            break;
        case SYSCTL_CLK_PDMA_CLK:
            if(enable == true)
                sysctl_clk->shrm_clk_cfg |= (1 << 3);
            else
                sysctl_clk->shrm_clk_cfg &= ~(1 << 3);
            break;

        /*--------------------------- DDR CLOCK ------------------------------------*/
        case SYSCTL_CLK_DDRC_CORE_CLK:
            if(enable == true)
                sysctl_clk->ddr_clk_cfg |= (1 << 2);
            else
                sysctl_clk->ddr_clk_cfg &= ~(1 << 2);
            break;

        case SYSCTL_CLK_DDR_BYPASSPCLK:
            if(enable == true)
                sysctl_clk->ddr_clk_cfg |= (1 << 8);
            else
                sysctl_clk->ddr_clk_cfg &= ~(1 << 8);
            break;

        case SYSCTL_CLK_DDRC_PCLK:
            if(enable == true)
                sysctl_clk->ddr_clk_cfg |= (1 << 9);
            else
                sysctl_clk->ddr_clk_cfg &= ~(1 << 9);
            break;

        case SYSCTL_CLK_DDRC_P0_CLK:
            if(enable == true)
                sysctl_clk->ddr_clk_cfg |= (1 << 3);
            else
                sysctl_clk->ddr_clk_cfg &= ~(1 << 3);
            break;

        case SYSCTL_CLK_NOC_DDRC_P4_CLK:
            if(enable == true)
                sysctl_clk->ddr_clk_cfg |= (1 << 7);
            else
                sysctl_clk->ddr_clk_cfg &= ~(1 << 7);
            break;

        case SYSCTL_CLK_NOC_DDRC_P3_ACLK:
            if(enable == true)
                sysctl_clk->ddr_clk_cfg |= (1 << 6);
            else
                sysctl_clk->ddr_clk_cfg &= ~(1 << 6);
            break;

        case SYSCTL_CLK_NOC_DDRC_P2_ACLK:
            if(enable == true)
                sysctl_clk->ddr_clk_cfg |= (1 << 5);
            else
                sysctl_clk->ddr_clk_cfg &= ~(1 << 5);
            break;

        case SYSCTL_CLK_NOC_DDRC_P1_ACLK:
            if(enable == true)
                sysctl_clk->ddr_clk_cfg |= (1 << 4);
            else
                sysctl_clk->ddr_clk_cfg &= ~(1 << 4);
            break;

        /*--------------------------- ISP CLOCK ------------------------------------*/
        case SYSCTL_CLK_ISP_CFG_CLK:
            if(enable == true)
                sysctl_clk->isp_clken_cfg |= (1 << 0);
            else
                sysctl_clk->isp_clken_cfg &= ~(1 << 0);
            break;

        case SYSCTL_CLK_ISP_PCLK:
            if(enable == true)
                sysctl_clk->isp_clken_cfg |= (1 << 1);
            else
                sysctl_clk->isp_clken_cfg &= ~(1 << 1);
            break;
        case SYSCTL_CLK_CSI0_CFG_CLK:
            if(enable == true)
                sysctl_clk->isp_clken_cfg |= (1 << 2);
            else
                sysctl_clk->isp_clken_cfg &= ~(1 << 2);
            break;
        case SYSCTL_CLK_CSI1_CFG_CLK:
            if(enable == true)
                sysctl_clk->isp_clken_cfg |= (1 << 3);
            else
                sysctl_clk->isp_clken_cfg &= ~(1 << 3);
            break;
        case SYSCTL_CLK_CSI2_CFG_CLK:
            if(enable == true)
                sysctl_clk->isp_clken_cfg |= (1 << 4);
            else
                sysctl_clk->isp_clken_cfg &= ~(1 << 4);
            break;
        case SYSCTL_CLK_ISP_DPHY_PCLK:
            if(enable == true)
                sysctl_clk->isp_clken_cfg |= (1 << 5);
            else
                sysctl_clk->isp_clken_cfg &= ~(1 << 5);
            break;

        case SYSCTL_CLK_CSI0_PIXEL_CLK:
            if(enable == true)
                sysctl_clk->isp_clken_cfg |= (1 << 6);
            else
                sysctl_clk->isp_clken_cfg &= ~(1 << 6);
            break;

        case SYSCTL_CLK_CSI1_PIXEL_CLK:
            if(enable == true)
                sysctl_clk->isp_clken_cfg |= (1 << 7);
            else
                sysctl_clk->isp_clken_cfg &= ~(1 << 7);
            break;

        case SYSCTL_CLK_CSI2_PIXEL_CLK:
            if(enable == true)
                sysctl_clk->isp_clken_cfg |= (1 << 8);
            else
                sysctl_clk->isp_clken_cfg &= ~(1 << 8);
            break;

        case SYSCTL_CLK_CSI0_PCLK:
            if(enable == true)
                sysctl_clk->isp_clken_cfg |= (1 << 9);
            else
                sysctl_clk->isp_clken_cfg &= ~(1 << 9);
            break;
        case SYSCTL_CLK_CSI1_PCLK:
            if(enable == true)
                sysctl_clk->isp_clken_cfg |= (1 << 10);
            else
                sysctl_clk->isp_clken_cfg &= ~(1 << 10);
            break;
        case SYSCTL_CLK_CSI2_PCLK:
            if(enable == true)
                sysctl_clk->isp_clken_cfg |= (1 << 11);
            else
                sysctl_clk->isp_clken_cfg &= ~(1 << 11);
            break;

        case SYSCTL_CLK_MCLK0:
            if(enable == true)
                sysctl_clk->mclk_cfg |= (1 << 0);
            else
                sysctl_clk->mclk_cfg &= ~(1 << 0);
            break;

        case SYSCTL_CLK_MCLK1:
            if(enable == true)
                sysctl_clk->mclk_cfg |= (1 << 1);
            else
                sysctl_clk->mclk_cfg &= ~(1 << 1);
            break;

        case SYSCTL_CLK_MCLK2:
            if(enable == true)
                sysctl_clk->mclk_cfg |= (1 << 2);
            else
                sysctl_clk->mclk_cfg &= ~(1 << 2);
            break;

        case SYSCTL_CLK_ISP_CLK:
            if(enable == true)
                sysctl_clk->isp_clken_cfg |= (1 << 12);
            else
                sysctl_clk->isp_clken_cfg &= ~(1 << 12);
            break;

        case SYSCTL_CLK_ISP_MCM_CLK:
            if(enable == true)
                sysctl_clk->isp_clken_cfg |= (1 << 17);
            else
                sysctl_clk->isp_clken_cfg &= ~(1 << 17);
            break;

        case SYSCTL_CLK_ISP_HCLK:
            if(enable == true)
                sysctl_clk->isp_clken_cfg |= (1 << 13);
            else
                sysctl_clk->isp_clken_cfg &= ~(1 << 13);
            break;

        case SYSCTL_CLK_ISP_ACLK:
            if(enable == true)
                sysctl_clk->isp_clken_cfg |= (1 << 14);
            else
                sysctl_clk->isp_clken_cfg &= ~(1 << 14);
            break;


        case SYSCTL_CLK_ISPDWE_CLK:
            if(enable == true)
                sysctl_clk->isp_clken_cfg |= (1 << 15);
            else
                sysctl_clk->isp_clken_cfg &= ~(1 << 15);
            break;

        case SYSCTL_CLK_ISPVSE_CLK:
            if(enable == true)
                sysctl_clk->isp_clken_cfg |= (1 << 16);
            else
                sysctl_clk->isp_clken_cfg &= ~(1 << 16);
            break;

        case SYSCTL_CLK_ISP_DW_HCLK:
            if(enable == true)
                sysctl_clk->isp_clken_cfg |= (1 << 18);
            else
                sysctl_clk->isp_clken_cfg &= ~(1 << 18);
            break;
        case SYSCTL_CLK_ISP_DW_ACLK:
            if(enable == true)
                sysctl_clk->isp_clken_cfg |= (1 << 19);
            else
                sysctl_clk->isp_clken_cfg &= ~(1 << 19);
            break;

        /*--------------------------- DPU CLOCK ------------------------------------*/
        case SYSCTL_CLK_DPU_CLK:
            if(enable == true)
                sysctl_clk->dpu_clk_cfg |= (1 << 0);
            else
                sysctl_clk->dpu_clk_cfg &= ~(1 << 0);
            break;

        case SYSCTL_CLK_DPU_ACLK:
            if(enable == true)
                sysctl_clk->dpu_clk_cfg |= (1 << 1);
            else
                sysctl_clk->dpu_clk_cfg &= ~(1 << 1);
            break;

        case SYSCTL_CLK_DPU_PCLK:
            if(enable == true)
                sysctl_clk->dpu_clk_cfg |= (1 << 2);
            else
                sysctl_clk->dpu_clk_cfg &= ~(1 << 2);
            break;

        /*--------------------------- DISP CLOCK ------------------------------------*/
        case SYSCTL_CLK_DISP_HCLK:
            if(enable == true)
                sysctl_clk->vo_clk_cfg |= (1 << 0);
            else
                sysctl_clk->vo_clk_cfg &= ~(1 << 0);
            break;

        case SYSCTL_CLK_DISP_ACLK:
            if(enable == true)
                sysctl_clk->vo_clk_cfg |= (1 << 1);
            else
                sysctl_clk->vo_clk_cfg &= ~(1 << 1);
            break;

        case SYSCTL_CLK_DPIPCLK:
            if(enable == true)
                sysctl_clk->vo_clk_cfg |= (1 << 2);
            else
                sysctl_clk->vo_clk_cfg &= ~(1 << 2);
            break;

        case SYSCTL_CLK_DISP_REFCLK:
            if(enable == true)
                sysctl_clk->vo_clk_cfg |= (1 << 3);
            else
                sysctl_clk->vo_clk_cfg &= ~(1 << 3);
            break;

        case SYSCTL_CLK_DISP_CFGCLK:
            if(enable == true)
                sysctl_clk->vo_clk_cfg |= (1 << 4);
            else
                sysctl_clk->vo_clk_cfg &= ~(1 << 4);
            break;

        case SYSCTL_CLK_DISP_CLKEXT:
            if(enable == true)
                sysctl_clk->vo_clk_cfg |= (1 << 5);
            else
                sysctl_clk->vo_clk_cfg &= ~(1 << 5);
            break;

        case SYSCTL_CLK_CLK_GPU:
            if(enable == true)
                sysctl_clk->vo_clk_cfg |= (1 << 6);
            else
                sysctl_clk->vo_clk_cfg &= ~(1 << 6);
            break;
        case SYSCTL_CLK_DISP_VO_PCLK:
            if(enable == true)
                sysctl_clk->vo_clk_cfg |= (1 << 7);
            else
                sysctl_clk->vo_clk_cfg &= ~(1 << 7);
            break;
        case SYSCTL_CLK_DISP_DSI_PCLK:
            if(enable == true)
                sysctl_clk->vo_clk_cfg |= (1 << 8);
            else
                sysctl_clk->vo_clk_cfg &= ~(1 << 8);
            break;
        case SYSCTL_CLK_DISP_DSI_PHY_PCLK:
            if(enable == true)
                sysctl_clk->vo_clk_cfg |= (1 << 9);
            else
                sysctl_clk->vo_clk_cfg &= ~(1 << 9);
            break;
        case SYSCTL_CLK_DISP_GPU_HCLK:
            if(enable == true)
                sysctl_clk->vo_clk_cfg |= (1 << 10);
            else
                sysctl_clk->vo_clk_cfg &= ~(1 << 10);
            break;
        case SYSCTL_CLK_DISP_VO_ACLK:
            if(enable == true)
                sysctl_clk->vo_clk_cfg |= (1 << 11);
            else
                sysctl_clk->vo_clk_cfg &= ~(1 << 11);
            break;
        case SYSCTL_CLK_DISP_GPU_ACLK:
            if(enable == true)
                sysctl_clk->vo_clk_cfg |= (1 << 12);
            else
                sysctl_clk->vo_clk_cfg &= ~(1 << 12);
            break;

        /*--------------------------- SEC CLOCK ------------------------------------*/
        case SYSCTL_CLK_SEC_PCLK:
            if(enable == true)
                sysctl_clk->sec_clk_div |= (1 << 0);
            else
                sysctl_clk->sec_clk_div &= ~(1 << 0);
            break;

        case SYSCTL_CLK_FIXCLK:
            if(enable == true)
                sysctl_clk->sec_clk_div |= (1 << 5);
            else
                sysctl_clk->sec_clk_div &= ~(1 << 5);
            break;

        case SYSCTL_CLK_SEC_ACLK:
            if(enable == true)
                sysctl_clk->sec_clk_div |= (1 << 4);
            else
                sysctl_clk->sec_clk_div &= ~(1 << 4);
            break;

        /*--------------------------- USB TEST MODE CLOCK ------------------------------------*/
        /* The output names are low-level aliases of the one physical gate.
         * k230_pm tracks only USB_TEST_CLK_GATE as gateable, so this does not
         * create independent PM refcounts for bit 0.
         */
        case SYSCTL_CLK_USB_TEST_CLK_GATE:
        case SYSCTL_CLK_USB_CLK480:
        case SYSCTL_CLK_USB_CLK100:
            if(enable == true)
                sysctl_clk->usb_test_clk_div |= (1 << 0);
            else
                sysctl_clk->usb_test_clk_div &= ~(1 << 0);
            break;

        /*--------------------------- DPHY DFT MODE CLOCK ------------------------------------*/
        case SYSCTL_CLK_DPHY_TEST_CLK:
            if(enable == true)
                sysctl_clk->dphy_test_clk_div |= (1 << 0);
            else
                sysctl_clk->dphy_test_clk_div &= ~(1 << 0);
            break;

        /*--------------------------- SPI2AXI CLOCK ------------------------------------*/
        case SYSCTL_CLK_SPI2AXI_ACLK:
            if(enable == true)
                sysctl_clk->spi2axi_clk_div |= (1 << 0);
            else
                sysctl_clk->spi2axi_clk_div &= ~(1 << 0);
            break;

        default:
            break;
    }
}

/* 获取本时钟节点的enable状态 */
bool sysctl_clk_get_leaf_en(sysctl_clk_node_e leaf)
{
    switch(sysctl_clk_attribute(leaf))
    {
        case 0:
            return false;
        case 1:
        case 3:
            break;
    }

    switch(leaf)
    {
        /*--------------------------- CPU0 CLOCK ------------------------------------*/
        case SYSCTL_CLK_CPU0_CORE_CLK_ROOT:
            return true;
        case SYSCTL_CLK_CPU0_CORE_CLK:
            return (0 == ((sysctl_clk->cpu0_clk_cfg >> 0) & 0x1)) ? false : true;
        case SYSCTL_CLK_CPU0_PLICLK:
            return (0 == ((sysctl_clk->cpu0_clk_cfg >> 9) & 0x1)) ? false : true;
        case SYSCTL_CLK_CPU0_ACLK:
            return true;
        case SYSCTL_CLK_TDI_PCLK:
            return (0 == ((sysctl_clk->cpu0_clk_cfg >> 14) & 0x1)) ? false : true;
        case SYSCTL_CLK_CPU0_PCLK:
            return (0 == ((sysctl_clk->cpu0_clk_cfg >> 13) & 0x1)) ? false : true;


        /*--------------------------- CPU1 CLOCK ------------------------------------*/
        case SYSCTL_CLK_CPU1_CORE_CLK_SRC:
        case SYSCTL_CLK_CPU1_CORE_CLK_ROOT:
            return true;
        case SYSCTL_CLK_CPU1_CORE_CLK:
            return (0 == ((sysctl_clk->cpu1_clk_cfg >> 0) & 0x1)) ? false : true;
        case SYSCTL_CLK_CPU1_PLICLK:
            return (0 == ((sysctl_clk->cpu1_clk_cfg >> 15) & 0x1)) ? false : true;
        case SYSCTL_CLK_CPU1_ACLK:
            return true;
        case SYSCTL_CLK_CPU1_PCLK:
            return (0 == ((sysctl_clk->cpu1_clk_cfg >> 19) & 0x1)) ? false : true;

        /*--------------------------- AI CLOCK ------------------------------------*/
        case SYSCTL_CLK_AI_CORE_CLK:
            return (0 == ((sysctl_clk->ai_clk_cfg >> 0) & 0x1)) ? false : true;
        case SYSCTL_CLK_AI_ACLK:
            return (0 == ((sysctl_clk->ai_clk_cfg >> 10) & 0x1)) ? false : true;

        /*--------------------------- VPU CLOCK ------------------------------------*/
        case SYSCTL_CLK_VPU_CLK:
            return (0 == ((sysctl_clk->vpu_clk_cfg >> 0) & 0x1)) ? false : true;
        case SYSCTL_CLK_VPU_ACLK_ROOT:
            return true;
        case SYSCTL_CLK_VPU_ACLK:
            return (0 == ((sysctl_clk->vpu_clk_cfg >> 5) & 0x1)) ? false : true;
        case SYSCTL_CLK_VPU_CFG_CLK:
            return (0 == ((sysctl_clk->vpu_clk_cfg >> 10) & 0x1)) ? false : true;

        /*--------------------------- PMU CLOCK ------------------------------------*/
        case SYSCTL_CLK_PMU_PCLK:
            return (0 == ((sysctl_clk->pmu_clk_cfg >> 0) & 0x1)) ? false : true;

        /*--------------------------- HS CLOCK ------------------------------------*/
        case SYSCTL_CLK_NOC_HS_OSPI_HCLK:
            return true;
        case SYSCTL_CLK_HS_HCLK_HIGH:
            return (0 == ((sysctl_clk->hs_clken_cfg >> 1) & 0x1)) ? false : true;
        case SYSCTL_CLK_HS_HCLK:
            return (0 == ((sysctl_clk->hs_clken_cfg >> 0) & 0x1)) ? false : true;
        case SYSCTL_CLK_SD0_HCLK:
            return (0 == ((sysctl_clk->hs_clken_cfg >> 2) & 0x1)) ? false : true;
        case SYSCTL_CLK_SD1_HCLK:
            return (0 == ((sysctl_clk->hs_clken_cfg >> 3) & 0x1)) ? false : true;
        case SYSCTL_CLK_USB0_HCLK:
            return (0 == ((sysctl_clk->hs_clken_cfg >> 4) & 0x1)) ? false : true;
        case SYSCTL_CLK_USB1_HCLK:
            return (0 == ((sysctl_clk->hs_clken_cfg >> 5) & 0x1)) ? false : true;
        case SYSCTL_CLK_SSI0_HCLK:
            return (0 == ((sysctl_clk->hs_clken_cfg >> 6) & 0x1)) ? false : true;
        case SYSCTL_CLK_SSI1_HCLK:
            return (0 == ((sysctl_clk->hs_clken_cfg >> 7) & 0x1)) ? false : true;
        case SYSCTL_CLK_SSI2_HCLK:
            return (0 == ((sysctl_clk->hs_clken_cfg >> 8) & 0x1)) ? false : true;

        case SYSCTL_CLK_SSI0_ACLK:
            return (0 == ((sysctl_clk->hs_clken_cfg >> 27) & 0x1)) ? false : true;
        case SYSCTL_CLK_SSI1_CLK:
            return (0 == ((sysctl_clk->hs_clken_cfg >> 25) & 0x1)) ? false : true;
        case SYSCTL_CLK_SSI2_CLK:
            return (0 == ((sysctl_clk->hs_clken_cfg >> 26) & 0x1)) ? false : true;
        case SYSCTL_CLK_QSPI_ACLK:
            return (0 == ((sysctl_clk->hs_clken_cfg >> 28) & 0x1)) ? false : true;
        case SYSCTL_CLK_SSI1_ACLK:
            return (0 == ((sysctl_clk->hs_clken_cfg >> 29) & 0x1)) ? false : true;
        case SYSCTL_CLK_SSI2_ACLK:
            return (0 == ((sysctl_clk->hs_clken_cfg >> 30) & 0x1)) ? false : true;

        case SYSCTL_CLK_SSI0_CLK:
            return (0 == ((sysctl_clk->hs_clken_cfg >> 24) & 0x1)) ? false : true;

        case SYSCTL_CLK_NOC_HS_SD_ACLK:
            return true;
        case SYSCTL_CLK_SD_ACLK:
            return (0 == ((sysctl_clk->hs_clken_cfg >> 9) & 0x1)) ? false : true;
        case SYSCTL_CLK_SD0_ACLK:
            return (0 == ((sysctl_clk->hs_clken_cfg >> 13) & 0x1)) ? false : true;
        case SYSCTL_CLK_SD1_ACLK:
            return (0 == ((sysctl_clk->hs_clken_cfg >> 17) & 0x1)) ? false : true;
        case SYSCTL_CLK_SD_BCLK:
            return (0 == ((sysctl_clk->hs_clken_cfg >> 10) & 0x1)) ? false : true;
        case SYSCTL_CLK_SD0_BCLK:
            return (0 == ((sysctl_clk->hs_clken_cfg >> 14) & 0x1)) ? false : true;
        case SYSCTL_CLK_SD1_BCLK:
            return (0 == ((sysctl_clk->hs_clken_cfg >> 18) & 0x1)) ? false : true;

        case SYSCTL_CLK_SD_CCLK:
            return (0 == ((sysctl_clk->hs_clken_cfg >> 11) & 0x1)) ? false : true;
        case SYSCTL_CLK_SD0_CCLK_TX:
            return (0 == ((sysctl_clk->hs_clken_cfg >> 15) & 0x1)) ? false : true;
        case SYSCTL_CLK_SD1_CCLK_TX:
            return (0 == ((sysctl_clk->hs_clken_cfg >> 19) & 0x1)) ? false : true;

        case SYSCTL_CLK_PLL0_OCLK_DIV16:
            return true;
        case SYSCTL_CLK_USB_REFCLK_50M:
            return true;
        case SYSCTL_CLK_USB0_REFCLK:
            return (0 == ((sysctl_clk->hs_clken_cfg >> 21) & 0x1)) ? false : true;
        case SYSCTL_CLK_USB1_REFCLK:
            return (0 == ((sysctl_clk->hs_clken_cfg >> 22) & 0x1)) ? false : true;

        case SYSCTL_CLK_SD_TMCLK:
            return (0 == ((sysctl_clk->hs_clken_cfg >> 12) & 0x1)) ? false : true;
        case SYSCTL_CLK_SD0_TMCLK:
            return (0 == ((sysctl_clk->hs_clken_cfg >> 16) & 0x1)) ? false : true;
        case SYSCTL_CLK_SD1_TMCLK:
            return (0 == ((sysctl_clk->hs_clken_cfg >> 20) & 0x1)) ? false : true;


        /*--------------------------- LS CLOCK ------------------------------------*/
        case SYSCTL_CLK_LS_PCLK:
            return (0 == ((sysctl_clk->ls_clken_cfg0 >> 0) & 0x1)) ? false : true;
        case SYSCTL_CLK_UART0_PCLK:
            return (0 == ((sysctl_clk->ls_clken_cfg0 >> 1) & 0x1)) ? false : true;
        case SYSCTL_CLK_UART1_PCLK:
            return (0 == ((sysctl_clk->ls_clken_cfg0 >> 2) & 0x1)) ? false : true;
        case SYSCTL_CLK_UART2_PCLK:
            return (0 == ((sysctl_clk->ls_clken_cfg0 >> 3) & 0x1)) ? false : true;
        case SYSCTL_CLK_UART3_PCLK:
            return (0 == ((sysctl_clk->ls_clken_cfg0 >> 4) & 0x1)) ? false : true;
        case SYSCTL_CLK_UART4_PCLK:
            return (0 == ((sysctl_clk->ls_clken_cfg0 >> 5) & 0x1)) ? false : true;
        case SYSCTL_CLK_I2C0_PCLK:
            return (0 == ((sysctl_clk->ls_clken_cfg0 >> 6) & 0x1)) ? false : true;
        case SYSCTL_CLK_I2C1_PCLK:
            return (0 == ((sysctl_clk->ls_clken_cfg0 >> 7) & 0x1)) ? false : true;
        case SYSCTL_CLK_I2C2_PCLK:
            return (0 == ((sysctl_clk->ls_clken_cfg0 >> 8) & 0x1)) ? false : true;
        case SYSCTL_CLK_I2C3_PCLK:
            return (0 == ((sysctl_clk->ls_clken_cfg0 >> 9) & 0x1)) ? false : true;
        case SYSCTL_CLK_I2C4_PCLK:
            return (0 == ((sysctl_clk->ls_clken_cfg0 >> 10) & 0x1)) ? false : true;
        case SYSCTL_CLK_GPIO_PCLK:
            return (0 == ((sysctl_clk->ls_clken_cfg0 >> 11) & 0x1)) ? false : true;
        case SYSCTL_CLK_PWM_PCLK:
            return (0 == ((sysctl_clk->ls_clken_cfg0 >> 12) & 0x1)) ? false : true;
        case SYSCTL_CLK_JAMLINK0_PCLK:
            return (0 == ((sysctl_clk->ls_clken_cfg1 >> 4) & 0x1)) ? false : true;
        case SYSCTL_CLK_JAMLINK1_PCLK:
            return (0 == ((sysctl_clk->ls_clken_cfg1 >> 5) & 0x1)) ? false : true;
        case SYSCTL_CLK_JAMLINK2_PCLK:
            return (0 == ((sysctl_clk->ls_clken_cfg1 >> 6) & 0x1)) ? false : true;
        case SYSCTL_CLK_JAMLINK3_PCLK:
            return (0 == ((sysctl_clk->ls_clken_cfg1 >> 7) & 0x1)) ? false : true;
        case SYSCTL_CLK_ADC_PCLK:
            return (0 == ((sysctl_clk->ls_clken_cfg0 >> 15) & 0x1)) ? false : true;

        case SYSCTL_CLK_UART0_CLK:
            return (0 == ((sysctl_clk->ls_clken_cfg0 >> 16) & 0x1)) ? false : true;
        case SYSCTL_CLK_UART1_CLK:
            return (0 == ((sysctl_clk->ls_clken_cfg0 >> 17) & 0x1)) ? false : true;
        case SYSCTL_CLK_UART2_CLK:
            return (0 == ((sysctl_clk->ls_clken_cfg0 >> 18) & 0x1)) ? false : true;
        case SYSCTL_CLK_UART3_CLK:
            return (0 == ((sysctl_clk->ls_clken_cfg0 >> 19) & 0x1)) ? false : true;
        case SYSCTL_CLK_UART4_CLK:
            return (0 == ((sysctl_clk->ls_clken_cfg0 >> 20) & 0x1)) ? false : true;

        case SYSCTL_CLK_JAMLINK0_CO:
            return (0 == ((sysctl_clk->ls_clken_cfg1 >> 0) & 0x1)) ? false : true;
        case SYSCTL_CLK_JAMLINK1_CO:
            return (0 == ((sysctl_clk->ls_clken_cfg1 >> 1) & 0x1)) ? false : true;
        case SYSCTL_CLK_JAMLINK2_CO:
            return (0 == ((sysctl_clk->ls_clken_cfg1 >> 2) & 0x1)) ? false : true;
        case SYSCTL_CLK_JAMLINK3_CO:
            return (0 == ((sysctl_clk->ls_clken_cfg1 >> 3) & 0x1)) ? false : true;

        case SYSCTL_CLK_I2C0_CLK:
            return (0 == ((sysctl_clk->ls_clken_cfg0 >> 21) & 0x1)) ? false : true;
        case SYSCTL_CLK_I2C1_CLK:
            return (0 == ((sysctl_clk->ls_clken_cfg0 >> 22) & 0x1)) ? false : true;
        case SYSCTL_CLK_I2C2_CLK:
            return (0 == ((sysctl_clk->ls_clken_cfg0 >> 23) & 0x1)) ? false : true;
        case SYSCTL_CLK_I2C3_CLK:
            return (0 == ((sysctl_clk->ls_clken_cfg0 >> 24) & 0x1)) ? false : true;
        case SYSCTL_CLK_I2C4_CLK:
            return (0 == ((sysctl_clk->ls_clken_cfg0 >> 25) & 0x1)) ? false : true;

        case SYSCTL_CLK_ADC_CLK:
            return (0 == ((sysctl_clk->ls_clken_cfg0 >> 26) & 0x1)) ? false : true;

        case SYSCTL_CLK_GPIO_DBCLK:
            return (0 == ((sysctl_clk->ls_clken_cfg0 >> 27) & 0x1)) ? false : true;

        case SYSCTL_CLK_AUDIO_PCLK:
            return (0 == ((sysctl_clk->ls_clken_cfg0 >> 13) & 0x1)) ? false : true;
        case SYSCTL_CLK_CODEC_PCLK:
            return (0 == ((sysctl_clk->ls_clken_cfg0 >> 14) & 0x1)) ? false : true;

        /*--------------------------- AUDIO CLOCK ------------------------------------*/

        case SYSCTL_CLK_CODEC_ADC_MCLK:
            return (0 == ((sysctl_clk->ls_clken_cfg0 >> 29) & 0x1)) ? false : true;
        case SYSCTL_CLK_CODEC_DAC_MCLK:
            return (0 == ((sysctl_clk->ls_clken_cfg0 >> 30) & 0x1)) ? false : true;
        case SYSCTL_CLK_AUD_DEV_CLK:
            return (0 == ((sysctl_clk->ls_clken_cfg0 >> 28) & 0x1)) ? false : true;
        case SYSCTL_CLK_PDM_CLK_ROOT:
            return (0 == ((sysctl_clk->ls_clken_cfg0 >> 31) & 0x1)) ? false : true;

        /*--------------------------- SYSCTL CLOCK ------------------------------------*/
        case SYSCTL_CLK_SYSCTL_PCLK:
            return true;
        case SYSCTL_CLK_WDT0_PCLK:
            return (0 == ((sysctl_clk->sysctl_clken_cfg >> 1) & 0x1)) ? false : true;
        case SYSCTL_CLK_WDT1_PCLK:
            return (0 == ((sysctl_clk->sysctl_clken_cfg >> 2) & 0x1)) ? false : true;
        case SYSCTL_CLK_TIMER_PCLK:
            return (0 == ((sysctl_clk->sysctl_clken_cfg >> 3) & 0x1)) ? false : true;
        case SYSCTL_CLK_IOMUX_PCLK:
            return (0 == ((sysctl_clk->sysctl_clken_cfg >> 20) & 0x1)) ? false : true;
        case SYSCTL_CLK_MAILBOX_PCLK:
            return (0 == ((sysctl_clk->sysctl_clken_cfg >> 4) & 0x1)) ? false : true;

        case SYSCTL_CLK_HDI_CLK:
            return (0 == ((sysctl_clk->sysctl_clken_cfg >> 21) & 0x1)) ? false : true;

        case SYSCTL_CLK_STC_CLK:
            return (0 == ((sysctl_clk->sysctl_clken_cfg >> 19) & 0x1)) ? false : true;

        case SYSCTL_CLK_TS_CLK:
            return true;

        case SYSCTL_CLK_WDT0_CLK:
            return (0 == ((sysctl_clk->sysctl_clken_cfg >> 5) & 0x1)) ? false : true;
        case SYSCTL_CLK_WDT1_CLK:
            return (0 == ((sysctl_clk->sysctl_clken_cfg >> 6) & 0x1)) ? false : true;

        /*--------------------------- TIMER CLOCK ------------------------------------*/
        case SYSCTL_CLK_TIMERX_PULSE_IN:
            return true;
        case SYSCTL_CLK_TIMER0_CLK:
            return (0 == ((sysctl_clk->sysctl_clken_cfg >> 13) & 0x1)) ? false : true;
        case SYSCTL_CLK_TIMER1_CLK:
            return (0 == ((sysctl_clk->sysctl_clken_cfg >> 14) & 0x1)) ? false : true;
        case SYSCTL_CLK_TIMER2_CLK:
            return (0 == ((sysctl_clk->sysctl_clken_cfg >> 15) & 0x1)) ? false : true;
        case SYSCTL_CLK_TIMER3_CLK:
            return (0 == ((sysctl_clk->sysctl_clken_cfg >> 16) & 0x1)) ? false : true;
        case SYSCTL_CLK_TIMER4_CLK:
            return (0 == ((sysctl_clk->sysctl_clken_cfg >> 17) & 0x1)) ? false : true;
        case SYSCTL_CLK_TIMER5_CLK:
            return (0 == ((sysctl_clk->sysctl_clken_cfg >> 18) & 0x1)) ? false : true;

        /*--------------------------- SHRM CLOCK ------------------------------------*/
        case SYSCTL_CLK_SRAM_CLK:
            return (0 == ((sysctl_clk->shrm_clk_cfg >> 10) & 0x1)) ? false : true;
        case SYSCTL_CLK_SHRM_DIV2:
            return true;
        case SYSCTL_CLK_DECOMP_PCLK:
            return (0 == ((sysctl_clk->shrm_clk_cfg >> 6) & 0x1)) ? false : true;
        case SYSCTL_CLK_SHRM_AXIM_CLK:
            return (0 == ((sysctl_clk->shrm_clk_cfg >> 13) & 0x1)) ? false : true;
        case SYSCTL_CLK_SHRM_AXIS_CLK:
            return (0 == ((sysctl_clk->shrm_clk_cfg >> 11) & 0x1)) ? false : true;
        case SYSCTL_CLK_DECOMP_CLK:
            return (0 == ((sysctl_clk->shrm_clk_cfg >> 7) & 0x1)) ? false : true;

        case SYSCTL_CLK_SHRM_PCLK:
            return (0 == ((sysctl_clk->shrm_clk_cfg >> 0) & 0x1)) ? false : true;

        case SYSCTL_CLK_NOC_SHRM_MCLK:
            return (0 == ((sysctl_clk->shrm_clk_cfg >> 12) & 0x1)) ? false : true;
        case SYSCTL_CLK_GSDMA_PCLK:
            return (0 == ((sysctl_clk->shrm_clk_cfg >> 4) & 0x1)) ? false : true;
        case SYSCTL_CLK_GSDMA_CLK:
            return (0 == ((sysctl_clk->shrm_clk_cfg >> 5) & 0x1)) ? false : true;
        case SYSCTL_CLK_NOAI_2D_PCLK:
            return (0 == ((sysctl_clk->shrm_clk_cfg >> 8) & 0x1)) ? false : true;
        case SYSCTL_CLK_NOAI_2D_CLK:
            return (0 == ((sysctl_clk->shrm_clk_cfg >> 9) & 0x1)) ? false : true;
        case SYSCTL_CLK_PDMA_PCLK:
            return (0 == ((sysctl_clk->shrm_clk_cfg >> 2) & 0x1)) ? false : true;
        case SYSCTL_CLK_PDMA_CLK:
            return (0 == ((sysctl_clk->shrm_clk_cfg >> 3) & 0x1)) ? false : true;

        /*--------------------------- DDR CLOCK ------------------------------------*/
        case SYSCTL_CLK_DDRC_CORE_CLK:
            return (0 == ((sysctl_clk->ddr_clk_cfg >> 2) & 0x1)) ? false : true;
        case SYSCTL_CLK_DDR_BYPASSPCLK:
            return (0 == ((sysctl_clk->ddr_clk_cfg >> 8) & 0x1)) ? false : true;
        case SYSCTL_CLK_DDRC_PCLK:
            return (0 == ((sysctl_clk->ddr_clk_cfg >> 9) & 0x1)) ? false : true;
        case SYSCTL_CLK_DDRC_P0_CLK:
            return (0 == ((sysctl_clk->ddr_clk_cfg >> 3) & 0x1)) ? false : true;
        case SYSCTL_CLK_NOC_DDRC_P4_CLK:
            return (0 == ((sysctl_clk->ddr_clk_cfg >> 7) & 0x1)) ? false : true;
        case SYSCTL_CLK_NOC_DDRC_P3_ACLK:
            return (0 == ((sysctl_clk->ddr_clk_cfg >> 6) & 0x1)) ? false : true;
        case SYSCTL_CLK_NOC_DDRC_P2_ACLK:
            return (0 == ((sysctl_clk->ddr_clk_cfg >> 5) & 0x1)) ? false : true;
        case SYSCTL_CLK_NOC_DDRC_P1_ACLK:
            return (0 == ((sysctl_clk->ddr_clk_cfg >> 4) & 0x1)) ? false : true;

        /*--------------------------- ISP CLOCK ------------------------------------*/
        case SYSCTL_CLK_ISP_CFG_CLK:
            return (0 == ((sysctl_clk->isp_clken_cfg >> 0) & 0x1)) ? false : true;
        case SYSCTL_CLK_ISP_PCLK:
            return (0 == ((sysctl_clk->isp_clken_cfg >> 1) & 0x1)) ? false : true;
        case SYSCTL_CLK_CSI0_CFG_CLK:
            return (0 == ((sysctl_clk->isp_clken_cfg >> 2) & 0x1)) ? false : true;
        case SYSCTL_CLK_CSI1_CFG_CLK:
            return (0 == ((sysctl_clk->isp_clken_cfg >> 3) & 0x1)) ? false : true;
        case SYSCTL_CLK_CSI2_CFG_CLK:
            return (0 == ((sysctl_clk->isp_clken_cfg >> 4) & 0x1)) ? false : true;
        case SYSCTL_CLK_ISP_DPHY_PCLK:
            return (0 == ((sysctl_clk->isp_clken_cfg >> 5) & 0x1)) ? false : true;
        case SYSCTL_CLK_CSI0_PIXEL_CLK:
            return (0 == ((sysctl_clk->isp_clken_cfg >> 6) & 0x1)) ? false : true;
        case SYSCTL_CLK_CSI1_PIXEL_CLK:
            return (0 == ((sysctl_clk->isp_clken_cfg >> 7) & 0x1)) ? false : true;
        case SYSCTL_CLK_CSI2_PIXEL_CLK:
            return (0 == ((sysctl_clk->isp_clken_cfg >> 8) & 0x1)) ? false : true;
        case SYSCTL_CLK_CSI0_PCLK:
            return (0 == ((sysctl_clk->isp_clken_cfg >> 9) & 0x1)) ? false : true;
        case SYSCTL_CLK_CSI1_PCLK:
            return (0 == ((sysctl_clk->isp_clken_cfg >> 10) & 0x1)) ? false : true;
        case SYSCTL_CLK_CSI2_PCLK:
            return (0 == ((sysctl_clk->isp_clken_cfg >> 11) & 0x1)) ? false : true;

        case SYSCTL_CLK_MCLK0:
            return (0 == ((sysctl_clk->mclk_cfg >> 0) & 0x1)) ? false : true;
        case SYSCTL_CLK_MCLK1:
            return (0 == ((sysctl_clk->mclk_cfg >> 1) & 0x1)) ? false : true;
        case SYSCTL_CLK_MCLK2:
            return (0 == ((sysctl_clk->mclk_cfg >> 2) & 0x1)) ? false : true;

        case SYSCTL_CLK_ISP_CLK:
            return (0 == ((sysctl_clk->isp_clken_cfg >> 12) & 0x1)) ? false : true;
        case SYSCTL_CLK_ISP_MCM_CLK:
            return (0 == ((sysctl_clk->isp_clken_cfg >> 17) & 0x1)) ? false : true;

        case SYSCTL_CLK_ISP_HCLK:
            return (0 == ((sysctl_clk->isp_clken_cfg >> 13) & 0x1)) ? false : true;

        case SYSCTL_CLK_ISP_ACLK:
            return (0 == ((sysctl_clk->isp_clken_cfg >> 14) & 0x1)) ? false : true;

        case SYSCTL_CLK_ISPDWE_CLK:
            return (0 == ((sysctl_clk->isp_clken_cfg >> 15) & 0x1)) ? false : true;
        case SYSCTL_CLK_ISPVSE_CLK:
            return (0 == ((sysctl_clk->isp_clken_cfg >> 16) & 0x1)) ? false : true;
        case SYSCTL_CLK_ISP_DW_HCLK:
            return (0 == ((sysctl_clk->isp_clken_cfg >> 18) & 0x1)) ? false : true;
        case SYSCTL_CLK_ISP_DW_ACLK:
            return (0 == ((sysctl_clk->isp_clken_cfg >> 19) & 0x1)) ? false : true;

        /*--------------------------- DPU CLOCK ------------------------------------*/
        case SYSCTL_CLK_DPU_CLK:
            return (0 == ((sysctl_clk->dpu_clk_cfg >> 0) & 0x1)) ? false : true;
        case SYSCTL_CLK_DPU_ACLK:
            return (0 == ((sysctl_clk->dpu_clk_cfg >> 1) & 0x1)) ? false : true;
        case SYSCTL_CLK_DPU_PCLK:
            return (0 == ((sysctl_clk->dpu_clk_cfg >> 2) & 0x1)) ? false : true;

        /*--------------------------- DISP CLOCK ------------------------------------*/
        case SYSCTL_CLK_DISP_HCLK:
            return (0 == ((sysctl_clk->vo_clk_cfg >> 0) & 0x1)) ? false : true;
        case SYSCTL_CLK_DISP_ACLK:
            return (0 == ((sysctl_clk->vo_clk_cfg >> 1) & 0x1)) ? false : true;
        case SYSCTL_CLK_DPIPCLK:
            return (0 == ((sysctl_clk->vo_clk_cfg >> 2) & 0x1)) ? false : true;
        case SYSCTL_CLK_DISP_REFCLK:
            return (0 == ((sysctl_clk->vo_clk_cfg >> 3) & 0x1)) ? false : true;
        case SYSCTL_CLK_DISP_CFGCLK:
            return (0 == ((sysctl_clk->vo_clk_cfg >> 4) & 0x1)) ? false : true;
        case SYSCTL_CLK_DISP_CLKEXT:
            return (0 == ((sysctl_clk->vo_clk_cfg >> 5) & 0x1)) ? false : true;
        case SYSCTL_CLK_CLK_GPU:
            return (0 == ((sysctl_clk->vo_clk_cfg >> 6) & 0x1)) ? false : true;
        case SYSCTL_CLK_DISP_VO_PCLK:
            return (0 == ((sysctl_clk->vo_clk_cfg >> 7) & 0x1)) ? false : true;
        case SYSCTL_CLK_DISP_DSI_PCLK:
            return (0 == ((sysctl_clk->vo_clk_cfg >> 8) & 0x1)) ? false : true;
        case SYSCTL_CLK_DISP_DSI_PHY_PCLK:
            return (0 == ((sysctl_clk->vo_clk_cfg >> 9) & 0x1)) ? false : true;
        case SYSCTL_CLK_DISP_GPU_HCLK:
            return (0 == ((sysctl_clk->vo_clk_cfg >> 10) & 0x1)) ? false : true;
        case SYSCTL_CLK_DISP_VO_ACLK:
            return (0 == ((sysctl_clk->vo_clk_cfg >> 11) & 0x1)) ? false : true;
        case SYSCTL_CLK_DISP_GPU_ACLK:
            return (0 == ((sysctl_clk->vo_clk_cfg >> 12) & 0x1)) ? false : true;

        /*--------------------------- SEC CLOCK ------------------------------------*/
        case SYSCTL_CLK_SEC_PCLK:
            return (0 == ((sysctl_clk->sec_clk_div >> 0) & 0x1)) ? false : true;
        case SYSCTL_CLK_FIXCLK:
            return (0 == ((sysctl_clk->sec_clk_div >> 5) & 0x1)) ? false : true;

        case SYSCTL_CLK_SEC_ACLK:
            return (0 == ((sysctl_clk->sec_clk_div >> 4) & 0x1)) ? false : true;

        /*--------------------------- USB TEST MODE CLOCK ------------------------------------*/
        case SYSCTL_CLK_USB_TEST_CLK_GATE:
        case SYSCTL_CLK_USB_CLK480:
            return (0 == ((sysctl_clk->usb_test_clk_div >> 0) & 0x1)) ? false : true;
        case SYSCTL_CLK_USB_CLK100:
            return (0 == ((sysctl_clk->usb_test_clk_div >> 0) & 0x1)) ? false : true;

        /*--------------------------- DPHY DFT MODE CLOCK ------------------------------------*/
        case SYSCTL_CLK_DPHY_TEST_CLK:
            return (0 == ((sysctl_clk->dphy_test_clk_div >> 0) & 0x1)) ? false : true;

        /*--------------------------- SPI2AXI CLOCK ------------------------------------*/
        case SYSCTL_CLK_SPI2AXI_ACLK:
            return (0 == ((sysctl_clk->spi2axi_clk_div >> 0) & 0x1)) ? false : true;

        default:
            return true;
    }
}

/* 设置本时钟节点的分频系数, freq = root_freq * numerator / denominator */
bool sysctl_clk_set_leaf_div(sysctl_clk_node_e leaf, uint32_t numerator, uint32_t denominator)
{
    volatile uint32_t ret;

    if(denominator == 0)
        return false;

    switch(leaf)
    {
        /*--------------------------- CPU0 CLOCK ------------------------------------*/
        case SYSCTL_CLK_CPU0_CORE_CLK_ROOT:
        {
            if((numerator > 16) || (numerator < 1) || (denominator != 16))
                return false;
            else
            {
                /* 1/16 --- 16/16 */
                ret = sysctl_clk->cpu0_clk_cfg;
                ret &= 0xffffffe1;
                sysctl_clk->cpu0_clk_cfg = ret | (((numerator - 1) << 1) | (1 << 31));
                return true;
            }
        }
        case SYSCTL_CLK_CPU0_PLICLK:
        {
            if((numerator != 1) || (denominator < 1) || (denominator > 8))
                return false;
            else
            {
                /* 1/1, 1/2, 1/3 --- 1/8 */
                ret = sysctl_clk->cpu0_clk_cfg;
                ret &= 0xffffe3ff;
                sysctl_clk->cpu0_clk_cfg = ret | (((denominator - 1) << 10) | (1 << 31));
                return true;
            }
        }
        case SYSCTL_CLK_CPU0_ACLK:
        {
            if((numerator != 1) || (denominator < 1) || (denominator > 8))
                return false;
            else
            {
                /* 1/1, 1/2, 1/3 --- 1/8 */
                ret = sysctl_clk->cpu0_clk_cfg;
                ret &= 0xfffffe3f;
                sysctl_clk->cpu0_clk_cfg = ret | (((denominator - 1) << 6) | (1 << 31));
                return true;
            }
        }

        case SYSCTL_CLK_TDI_PCLK:
        case SYSCTL_CLK_CPU0_PCLK:
        {
            if((numerator != 1) || (denominator < 1) || (denominator > 8))
                return false;
            else
            {
                /* 1/1, 1/2, 1/3 --- 1/8 */
                ret = sysctl_clk->cpu0_clk_cfg;
                ret &= 0xffffc7ff;
                sysctl_clk->cpu0_clk_cfg = ret | (((denominator - 1) << 15) | (1 << 31));
                return true;
            }
        }

        /*--------------------------- CPU1 CLOCK ------------------------------------*/
        case SYSCTL_CLK_CPU1_CORE_CLK_ROOT:
        {
            if((numerator != 1) || (denominator < 1) || (denominator > 8))
                return false;
            else
            {
                /* 1/1, 1/2, 1/3 --- 1/8 */
                ret = sysctl_clk->cpu1_clk_cfg;
                ret &= 0xffffffc7;
                sysctl_clk->cpu1_clk_cfg = ret | (((denominator - 1) << 3) | (1 << 31));
                return true;
            }
        }
        case SYSCTL_CLK_CPU1_PLICLK:
        {
            if((numerator != 1) || (denominator < 1) || (denominator > 8))
                return false;
            else
            {
                /* 1/1, 1/2, 1/3 --- 1/8 */
                ret = sysctl_clk->cpu1_clk_cfg;
                ret &= 0xfff8ffff;
                sysctl_clk->cpu1_clk_cfg = ret | (((denominator - 1) << 16) | (1 << 31));
                return true;
            }
        }
        case SYSCTL_CLK_CPU1_ACLK:
        {
            if((numerator != 1) || (denominator < 1) || (denominator > 8))
                return false;
            else
            {
                /* 1/1, 1/2, 1/3 --- 1/8 */
                ret = sysctl_clk->cpu1_clk_cfg;
                ret &= 0xffff8fff;
                sysctl_clk->cpu1_clk_cfg = ret | (((denominator - 1) << 12) | (1 << 31));
                return true;
            }
        }

        case SYSCTL_CLK_CPU1_PCLK:
            return false;       /* CPU1_CLK_CFG has a gate, but no divider. */
        /*--------------------------- AI CLOCK ------------------------------------*/
        case SYSCTL_CLK_AI_CORE_CLK:
        {
            if((numerator != 1) || (denominator < 1) || (denominator > 8))
                return false;
            else
            {
                /* 1/1, 1/2, 1/3 --- 1/8 */
                ret = sysctl_clk->ai_clk_cfg;
                ret &= 0xffffffc7;
                sysctl_clk->ai_clk_cfg = ret | (((denominator - 1) << 3) | (1 << 31));
                return true;
            }
        }
        case SYSCTL_CLK_AI_ACLK:
            return false;

        /*--------------------------- VPU CLOCK ------------------------------------*/
        case SYSCTL_CLK_VPU_CLK_ROOT:
        {
            if((numerator > 16) || (numerator < 1) || (denominator != 16))
                return false;
            else
            {
                /* 1/16 --- 16/16 */
                ret = sysctl_clk->vpu_clk_cfg;
                ret &= 0xffffffe1;
                sysctl_clk->vpu_clk_cfg = ret | (((numerator - 1) << 1) | (1 << 31));
                return true;
            }
        }
        case SYSCTL_CLK_VPU_ACLK_ROOT:
        {
            if((numerator != 1) || (denominator < 1) || (denominator > 16))
                return false;
            else
            {
                /* 1/1, 1/2, 1/3 --- 1/16 */
                ret = sysctl_clk->vpu_clk_cfg;
                ret &= 0xfffffc3f;
                sysctl_clk->vpu_clk_cfg = ret | (((denominator - 1) << 6) | (1 << 31));
                return true;
            }
        }
        case SYSCTL_CLK_VPU_ACLK:
            return false;
        case SYSCTL_CLK_NOC_VPU_CFG_CLK:
        {
            if((numerator != 1) || (denominator < 1) || (denominator > 16))
                return false;
            else
            {
                /* 1/1, 1/2, 1/3 --- 1/16 */
                ret = sysctl_clk->vpu_clk_cfg;
                ret &= 0xffff87ff;
                sysctl_clk->vpu_clk_cfg = ret | (((denominator - 1) << 11) | (1 << 31));
                return true;
            }
        }

        /*--------------------------- PMU CLOCK ------------------------------------*/
        case SYSCTL_CLK_PMU_PCLK:
            return false;

        /*--------------------------- HS CLOCK ------------------------------------*/
        case SYSCTL_CLK_NOC_HS_OSPI_HCLK:
        {
            if((numerator != 1) || (denominator < 1) || (denominator > 8))
                return false;
            else
            {
                /* 1/1, 1/2, 1/3 --- 1/8 */
                ret = sysctl_clk->hs_sdclk_cfg;
                ret &= 0xfffffff8;
                sysctl_clk->hs_sdclk_cfg = ret | (((denominator - 1) << 0) | (1 << 31));
                return true;
            }
        }
        case SYSCTL_CLK_HS_HCLK_HIGH:
            return false;

        case SYSCTL_CLK_NOC_HS_SUB_HCLK:
        {
            if((numerator != 1) || (denominator < 1) || (denominator > 8))
                return false;
            else
            {
                /* 1/1, 1/2, 1/3 --- 1/8 */
                ret = sysctl_clk->hs_sdclk_cfg;
                ret &= 0xffffffc7;
                sysctl_clk->hs_sdclk_cfg = ret | (((denominator - 1) << 3) | (1 << 31));
                return true;
            }
        }
        case SYSCTL_CLK_SD0_HCLK:
        case SYSCTL_CLK_SD1_HCLK:
        case SYSCTL_CLK_USB0_HCLK:
        case SYSCTL_CLK_USB1_HCLK:
        case SYSCTL_CLK_SSI0_HCLK:
        case SYSCTL_CLK_SSI1_HCLK:
        case SYSCTL_CLK_SSI2_HCLK:
            return false;

        case SYSCTL_CLK_NOC_HS_OSPI_ACLK:
        {
            if((numerator != 1) || (denominator < 1) || (denominator > 8))
                return false;
            else
            {
                /* 1/1, 1/2, 1/3 --- 1/8 */
                ret = sysctl_clk->hs_spi_cfg;
                ret &= 0xfffff1ff;
                sysctl_clk->hs_spi_cfg = ret | (((denominator - 1) << 9) | (1 << 31));
                return true;
            }
        }
        case SYSCTL_CLK_SSI1_CLK:
        {
            if((numerator != 1) || (denominator < 1) || (denominator > 8))
                return false;
            else
            {
                /* 1/1, 1/2, 1/3 --- 1/8 */
                ret = sysctl_clk->hs_spi_cfg;
                ret &= 0xffffffc7;
                sysctl_clk->hs_spi_cfg = ret | (((denominator - 1) << 3) | (1 << 31));
                return true;
            }
        }
        case SYSCTL_CLK_SSI2_CLK:
        {
            if((numerator != 1) || (denominator < 1) || (denominator > 8))
                return false;
            else
            {
                /* 1/1, 1/2, 1/3 --- 1/8 */
                ret = sysctl_clk->hs_spi_cfg;
                ret &= 0xfffffe3f;
                sysctl_clk->hs_spi_cfg = ret | (((denominator - 1) << 6) | (1 << 31));
                return true;
            }
        }
        case SYSCTL_CLK_NOC_HS_QSPI_ACLK:
        {
            if((numerator != 1) || (denominator < 1) || (denominator > 8))
                return false;
            else
            {
                /* 1/1, 1/2, 1/3 --- 1/8 */
                ret = sysctl_clk->hs_spi_cfg;
                ret &= 0xffff8fff;
                sysctl_clk->hs_spi_cfg = ret | (((denominator - 1) << 12) | (1 << 31));
                return true;
            }
        }
        case SYSCTL_CLK_SSI1_ACLK:
        case SYSCTL_CLK_SSI2_ACLK:
            return false;

        case SYSCTL_CLK_SSI0_CLK:
            return false;

        case SYSCTL_CLK_NOC_HS_SD_ACLK:
        {
            if((numerator != 1) || (denominator < 1) || (denominator > 8))
                return false;
            else
            {
                /* 1/1, 1/2, 1/3 --- 1/8 */
                ret = sysctl_clk->hs_sdclk_cfg;
                ret &= 0xfffffe3f;
                sysctl_clk->hs_sdclk_cfg = ret | (((denominator - 1) << 6) | (1 << 31));
                return true;
            }
        }
        case SYSCTL_CLK_SD_BCLK:
        {
            if((numerator != 1) || (denominator < 1) || (denominator > 8))
                return false;
            else
            {
                /* 1/1, 1/2, 1/3 --- 1/8 */
                ret = sysctl_clk->hs_sdclk_cfg;
                ret &= 0xfffff1ff;
                sysctl_clk->hs_sdclk_cfg = ret | (((denominator - 1) << 9) | (1 << 31));
                return true;
            }
        }
        case SYSCTL_CLK_SD0_ACLK:
        case SYSCTL_CLK_SD1_ACLK:
        case SYSCTL_CLK_SD0_BCLK:
        case SYSCTL_CLK_SD1_BCLK:
            return false;

        case SYSCTL_CLK_SD_CCLK:
        {
            if((numerator != 1) || (denominator < 1) || (denominator > 8))
                return false;
            else
            {
                /* 1/1, 1/2, 1/3 --- 1/8 */
                ret = sysctl_clk->hs_sdclk_cfg;
                ret &= 0xffff8fff;
                sysctl_clk->hs_sdclk_cfg = ret | (((denominator - 1) << 12) | (1 << 31));
                return true;
            }
        }
        case SYSCTL_CLK_SD0_CCLK_TX:
        case SYSCTL_CLK_SD1_CCLK_TX:
            return false;

        case SYSCTL_CLK_PLL0_OCLK_DIV16:
            return false;
        case SYSCTL_CLK_USB_REFCLK_50M:
        {
            if((numerator != 1) || (denominator < 1) || (denominator > 8))
                return false;
            else
            {
                /* 1/1, 1/2, 1/3 --- 1/8 */
                ret = sysctl_clk->hs_spi_cfg;
                ret &= 0xfffc7fff;
                sysctl_clk->hs_spi_cfg = ret | (((denominator - 1) << 15) | (1 << 31));
                return true;
            }
        }
        case SYSCTL_CLK_USB0_REFCLK:
        case SYSCTL_CLK_USB1_REFCLK:
            return false;

        case SYSCTL_CLK_SD_TMCLK:
        {
            if((numerator != 1) || (denominator < 1) || (denominator > 32))
                return false;
            else
            {
                /* 1/1, 1/2, 1/3 --- 1/32 */
                ret = sysctl_clk->hs_sdclk_cfg;
                ret &= 0xfff07fff;
                sysctl_clk->hs_sdclk_cfg = ret | (((denominator - 1) << 15) | (1 << 31));
                return true;
            }
        }
        case SYSCTL_CLK_SD0_TMCLK:
        case SYSCTL_CLK_SD1_TMCLK:
            return false;

        /*--------------------------- LS CLOCK ------------------------------------*/
        case SYSCTL_CLK_NOC_LS_PCLK:
        {
            if((numerator != 1) || (denominator < 1) || (denominator > 8))
                return false;
            else
            {
                /* 1/1, 1/2, 1/4, 1/8 --- 1/8 */
                ret = sysctl_clk->ls_clkdiv_cfg;
                ret &= 0xfffffff8;
                sysctl_clk->ls_clkdiv_cfg = ret | (((denominator - 1) << 0) | (1 << 31));
                return true;
            }
        }
        case SYSCTL_CLK_UART0_PCLK:
        case SYSCTL_CLK_UART1_PCLK:
        case SYSCTL_CLK_UART2_PCLK:
        case SYSCTL_CLK_UART3_PCLK:
        case SYSCTL_CLK_UART4_PCLK:
        case SYSCTL_CLK_I2C0_PCLK:
        case SYSCTL_CLK_I2C1_PCLK:
        case SYSCTL_CLK_I2C2_PCLK:
        case SYSCTL_CLK_I2C3_PCLK:
        case SYSCTL_CLK_I2C4_PCLK:
        case SYSCTL_CLK_GPIO_PCLK:
        case SYSCTL_CLK_PWM_PCLK:
        case SYSCTL_CLK_JAMLINK0_PCLK:
        case SYSCTL_CLK_JAMLINK1_PCLK:
        case SYSCTL_CLK_JAMLINK2_PCLK:
        case SYSCTL_CLK_JAMLINK3_PCLK:
        case SYSCTL_CLK_AUDIO_PCLK:
        case SYSCTL_CLK_ADC_PCLK:
        case SYSCTL_CLK_CODEC_PCLK:
            return false;

        case SYSCTL_CLK_UART0_CLK:
        {
            if((numerator != 1) || (denominator < 1) || (denominator > 8))
                return false;
            else
            {
                /* 1/1, 1/2, 1/3 --- 1/8 */
                ret = sysctl_clk->uart_i2c_clkdiv_cfg;
                ret &= 0xfffffff8;
                sysctl_clk->uart_i2c_clkdiv_cfg = ret | (((denominator - 1) << 0) | (1 << 31));
                return true;
            }
        }
        case SYSCTL_CLK_UART1_CLK:
        {
            if((numerator != 1) || (denominator < 1) || (denominator > 8))
                return false;
            else
            {
                /* 1/1, 1/2, 1/3 --- 1/8 */
                ret = sysctl_clk->uart_i2c_clkdiv_cfg;
                ret &= 0xffffffc7;
                sysctl_clk->uart_i2c_clkdiv_cfg = ret | (((denominator - 1) << 3) | (1 << 31));
                return true;
            }
        }
        case SYSCTL_CLK_UART2_CLK:
        {
            if((numerator != 1) || (denominator < 1) || (denominator > 8))
                return false;
            else
            {
                /* 1/1, 1/2, 1/3 --- 1/8 */
                ret = sysctl_clk->uart_i2c_clkdiv_cfg;
                ret &= 0xfffffe3f;
                sysctl_clk->uart_i2c_clkdiv_cfg = ret | (((denominator - 1) << 6) | (1 << 31));
                return true;
            }
        }
        case SYSCTL_CLK_UART3_CLK:
        {
            if((numerator != 1) || (denominator < 1) || (denominator > 8))
                return false;
            else
            {
                /* 1/1, 1/2, 1/3 --- 1/8 */
                ret = sysctl_clk->uart_i2c_clkdiv_cfg;
                ret &= 0xfffff1ff;
                sysctl_clk->uart_i2c_clkdiv_cfg = ret | (((denominator - 1) << 9) | (1 << 31));
                return true;
            }
        }
        case SYSCTL_CLK_UART4_CLK:
        {
            if((numerator != 1) || (denominator < 1) || (denominator > 8))
                return false;
            else
            {
                /* 1/1, 1/2, 1/3 --- 1/8 */
                ret = sysctl_clk->uart_i2c_clkdiv_cfg;
                ret &= 0xffff8fff;
                sysctl_clk->uart_i2c_clkdiv_cfg = ret | (((denominator - 1) << 12) | (1 << 31));
                return true;
            }
        }

        /* All four JAMLINK CO outputs share LS_CLKDIV_CFG[30:23]. */
        case SYSCTL_CLK_JAMLINK0_CO:
        case SYSCTL_CLK_JAMLINK1_CO:
        case SYSCTL_CLK_JAMLINK2_CO:
        case SYSCTL_CLK_JAMLINK3_CO:
        {
            if((numerator != 1) || (denominator < 2) || (denominator > 512) || (denominator % 2 != 0))
                return false;
            else
            {
                /* 1/2, 1/4, 1/8 --- 1/512 */
                ret = sysctl_clk->ls_clkdiv_cfg;
                ret &= 0x807fffff;
                sysctl_clk->ls_clkdiv_cfg = ret | (((denominator/2 - 1) << 23) | (1 << 31));
                return true;
            }
        }
        case SYSCTL_CLK_I2C0_CLK:
        {
            if((numerator != 1) || (denominator < 1) || (denominator > 8))
                return false;
            else
            {
                /* 1/1, 1/2, 1/3 --- 1/8 */
                ret = sysctl_clk->uart_i2c_clkdiv_cfg;
                ret &= 0xfffc7fff;
                sysctl_clk->uart_i2c_clkdiv_cfg = ret | (((denominator - 1) << 15) | (1 << 31));
                return true;
            }
        }
        case SYSCTL_CLK_I2C1_CLK:
        {
            if((numerator != 1) || (denominator < 1) || (denominator > 8))
                return false;
            else
            {
                /* 1/1, 1/2, 1/3 --- 1/8 */
                ret = sysctl_clk->uart_i2c_clkdiv_cfg;
                ret &= 0xffe3ffff;
                sysctl_clk->uart_i2c_clkdiv_cfg = ret | (((denominator - 1) << 18) | (1 << 31));
                return true;
            }
        }
        case SYSCTL_CLK_I2C2_CLK:
        {
            if((numerator != 1) || (denominator < 1) || (denominator > 8))
                return false;
            else
            {
                /* 1/1, 1/2, 1/3 --- 1/8 */
                ret = sysctl_clk->uart_i2c_clkdiv_cfg;
                ret &= 0xff1fffff;
                sysctl_clk->uart_i2c_clkdiv_cfg = ret | (((denominator - 1) << 21) | (1 << 31));
                return true;
            }
        }
        case SYSCTL_CLK_I2C3_CLK:
        {
            if((numerator != 1) || (denominator < 1) || (denominator > 8))
                return false;
            else
            {
                /* 1/1, 1/2, 1/3 --- 1/8 */
                ret = sysctl_clk->uart_i2c_clkdiv_cfg;
                ret &= 0xf8ffffff;
                sysctl_clk->uart_i2c_clkdiv_cfg = ret | (((denominator - 1) << 24) | (1 << 31));
                return true;
            }
        }
        case SYSCTL_CLK_I2C4_CLK:
        {
            if((numerator != 1) || (denominator < 1) || (denominator > 8))
                return false;
            else
            {
                /* 1/1, 1/2, 1/3 --- 1/8 */
                ret = sysctl_clk->uart_i2c_clkdiv_cfg;
                ret &= 0xc7ffffff;
                sysctl_clk->uart_i2c_clkdiv_cfg = ret | (((denominator - 1) << 27) | (1 << 31));
                return true;
            }
        }

        case SYSCTL_CLK_CODEC_ADC_MCLK:
        {
            if((numerator < 0x10) || (numerator > 0x1B9) || (denominator < 0xC35) || (denominator > 0x3D09))
                return false;
            if(numerator * 2 > denominator)      //根据文档描述，max_sum的设定值至少是inc_step设定值的2倍以上
                return false;

            ret = sysctl_clk->codec_adc_mclkdiv_cfg;
            ret &= 0xf8000000;
            sysctl_clk->codec_adc_mclkdiv_cfg = ret | ((numerator << 14) | (denominator << 0) | (1 << 31));
            return true;
        }
        case SYSCTL_CLK_CODEC_DAC_MCLK:
        {
            if((numerator < 0x10) || (numerator > 0x1B9) || (denominator < 0xC35) || (denominator > 0x3D09))
                return false;
            if(numerator * 2 > denominator)      //根据文档描述，max_sum的设定值至少是inc_step设定值的2倍以上
                return false;

            ret = sysctl_clk->codec_dac_mclkdiv_cfg;
            ret &= 0xf8000000;
            sysctl_clk->codec_dac_mclkdiv_cfg = ret | ((numerator << 14) | (denominator << 0) | (1 << 31));
            return true;
        }
        case SYSCTL_CLK_AUD_DEV_CLK:
        {
            if((numerator < 0x4) || (numerator > 0x1B9) || (denominator < 0xC35) || (denominator > 0xF424))
                return false;
            if(numerator * 2 > denominator)      //根据文档描述，max_sum的设定值至少是inc_step设定值的2倍以上
                return false;

            ret = sysctl_clk->audio_clkdiv_cfg;
            ret &= 0x80000000;
            sysctl_clk->audio_clkdiv_cfg = ret | ((numerator << 16) | (denominator << 0) | (1 << 31));
            // ret |= ((numerator << 16) | (denominator << 0));
            // ret |= (1 << 31);
            // sysctl_clk->audio_clkdiv_cfg = ret;
            return true;
        }

        //review。注意，此处需要与clock_provider.dtsi保持一致，dtsi可能会被改变。
        //后者不需要按照dtsi再修改，此处代码已经写好，即在配置寄存器的时候，配置两个寄存器即可。
        case SYSCTL_CLK_PDM_CLK_ROOT:
        {
            if((numerator < 0x2) || (numerator > 0x1B9) || (denominator < 0xC35) || (denominator > 0x1E848))
                return false;
            if(numerator * 2 > denominator)      //根据文档描述，max_sum的设定值至少是inc_step设定值的2倍以上
                return false;

            // sysctl_clk->pdm_clkdiv_cfg0 = ((numerator << 32) | (denominator << 0) | (1 << 63));
            sysctl_clk->pdm_clkdiv_cfg0 = (denominator << 0);
            sysctl_clk->pdm_clkdiv_cfg1 = ((numerator << 0) | (1 << 31));
            return true;
        }
        case SYSCTL_CLK_ADC_CLK:
        {
            if((numerator != 1) || (denominator < 1) || (denominator > 1024))
                return false;
            else
            {
                /* 1/1, 1/2, 1/3 --- 1/1024 */
                ret = sysctl_clk->ls_clkdiv_cfg;
                ret &= 0xffffe007;
                sysctl_clk->ls_clkdiv_cfg = ret | (((denominator - 1) << 3) | (1 << 31));
                return true;
            }
        }

        case SYSCTL_CLK_GPIO_DBCLK:
        {
            if((numerator != 1) || (denominator < 1) || (denominator > 1024))
                return false;
            else
            {
                /* 1/1, 1/2, 1/3 --- 1/1024 */
                ret = sysctl_clk->ls_clkdiv_cfg;
                ret &= 0xff801fff;
                sysctl_clk->ls_clkdiv_cfg = ret | (((denominator - 1) << 13) | (1 << 31));
                return true;
            }
        }

        /*--------------------------- SYSCTL CLOCK ------------------------------------*/
        case SYSCTL_CLK_SYSCTL_PCLK:
            return false;
        case SYSCTL_CLK_WDT0_PCLK:
        case SYSCTL_CLK_WDT1_PCLK:
        case SYSCTL_CLK_TIMER_PCLK:
        case SYSCTL_CLK_IOMUX_PCLK:
        case SYSCTL_CLK_MAILBOX_PCLK:
            return false;

        case SYSCTL_CLK_HDI_CLK:
        {
            if((numerator != 1) || (denominator < 1) || (denominator > 8))
                return false;
            else
            {
                /* 1/1, 1/2, 1/3 --- 1/8 */
                ret = sysctl_clk->sysctl_clk_div_cfg;
                ret &= 0x8fffffff;
                sysctl_clk->sysctl_clk_div_cfg = ret | (((denominator - 1) << 28) | (1 << 31));
                return true;
            }
        }

        case SYSCTL_CLK_STC_CLK:
        {
            if((numerator != 1) || (denominator < 1) || (denominator > 32))
                return false;
            else
            {
                /* 1/1, 1/2, 1/3 --- 1/32 */
                ret = sysctl_clk->sysctl_clk_div_cfg;
                ret &= 0xfff07fff;
                sysctl_clk->sysctl_clk_div_cfg = ret | (((denominator - 1) << 15) | (1 << 31));
                return true;
            }
        }

        case SYSCTL_CLK_TS_CLK:
        {
            if((numerator != 1) || (denominator < 1) || (denominator > 256))
                return false;
            else
            {
                /* 1/1, 1/2, 1/3 --- 1/256 */
                ret = sysctl_clk->sysctl_clk_div_cfg;
                ret &= 0xf00fffff;
                sysctl_clk->sysctl_clk_div_cfg = ret | (((denominator - 1) << 20) | (1 << 31));
                return true;
            }
        }

        case SYSCTL_CLK_WDT0_CLK:
        {
            if((numerator != 1) || (denominator < 1) || (denominator > 64))
                return false;
            else
            {
                /* 1/1, 1/2, 1/3 --- 1/64 */
                ret = sysctl_clk->sysctl_clk_div_cfg;
                ret &= 0xfffffe07;
                sysctl_clk->sysctl_clk_div_cfg = ret | (((denominator - 1) << 3) | (1 << 31));
                return true;
            }
        }

        case SYSCTL_CLK_WDT1_CLK:
        {
            if((numerator != 1) || (denominator < 1) || (denominator > 64))
                return false;
            else
            {
                /* 1/1, 1/2, 1/3 --- 1/64 */
                ret = sysctl_clk->sysctl_clk_div_cfg;
                ret &= 0xffff81ff;
                sysctl_clk->sysctl_clk_div_cfg = ret | (((denominator - 1) << 9) | (1 << 31));
                return true;
            }
        }

        /*--------------------------- TIMER CLOCK ------------------------------------*/
        case SYSCTL_CLK_TIMERX_PULSE_IN:
            return false;
        case SYSCTL_CLK_TIMER0_CLK:
        {
            if((numerator != 1) || (denominator < 1) || (denominator > 8))
                return false;
            else
            {
                /* 1/1, 1/2, 1/3 --- 1/8 */
                ret = sysctl_clk->timer_clk_cfg;
                ret &= 0xfffffff8;
                sysctl_clk->timer_clk_cfg = ret | (((denominator - 1) << 0) | (1 << 31));
                return true;
            }
        }
        case SYSCTL_CLK_TIMER1_CLK:
        {
            if((numerator != 1) || (denominator < 1) || (denominator > 8))
                return false;
            else
            {
                /* 1/1, 1/2, 1/3 --- 1/8 */
                ret = sysctl_clk->timer_clk_cfg;
                ret &= 0xffffffc7;
                sysctl_clk->timer_clk_cfg = ret | (((denominator - 1) << 3) | (1 << 31));
                return true;
            }
        }
        case SYSCTL_CLK_TIMER2_CLK:
        {
            if((numerator != 1) || (denominator < 1) || (denominator > 8))
                return false;
            else
            {
                /* 1/1, 1/2, 1/3 --- 1/8 */
                ret = sysctl_clk->timer_clk_cfg;
                ret &= 0xfffffe3f;
                sysctl_clk->timer_clk_cfg = ret | (((denominator - 1) << 6) | (1 << 31));
                return true;
            }
        }
        case SYSCTL_CLK_TIMER3_CLK:
        {
            if((numerator != 1) || (denominator < 1) || (denominator > 8))
                return false;
            else
            {
                /* 1/1, 1/2, 1/3 --- 1/8 */
                ret = sysctl_clk->timer_clk_cfg;
                ret &= 0xfffff1ff;
                sysctl_clk->timer_clk_cfg = ret | (((denominator - 1) << 9) | (1 << 31));
                return true;
            }
        }
        case SYSCTL_CLK_TIMER4_CLK:
        {
            if((numerator != 1) || (denominator < 1) || (denominator > 8))
                return false;
            else
            {
                /* 1/1, 1/2, 1/3 --- 1/8 */
                ret = sysctl_clk->timer_clk_cfg;
                ret &= 0xffff8fff;
                sysctl_clk->timer_clk_cfg = ret | (((denominator - 1) << 12) | (1 << 31));
                return true;
            }
        }
        case SYSCTL_CLK_TIMER5_CLK:
        {
            if((numerator != 1) || (denominator < 1) || (denominator > 8))
                return false;
            else
            {
                /* 1/1, 1/2, 1/3 --- 1/8 */
                ret = sysctl_clk->timer_clk_cfg;
                ret &= 0xfffc7fff;
                sysctl_clk->timer_clk_cfg = ret | (((denominator - 1) << 15) | (1 << 31));
                return true;
            }
        }
        /*--------------------------- SHRM CLOCK ------------------------------------*/
        case SYSCTL_CLK_SRAM_CLK:
            return false;
        case SYSCTL_CLK_SHRM_DIV2:
            return false;
        case SYSCTL_CLK_SHRM_AXIM_CLK:
        case SYSCTL_CLK_SHRM_AXIS_CLK:
        case SYSCTL_CLK_DECOMP_PCLK:
        case SYSCTL_CLK_DECOMP_CLK:
            return false;

        case SYSCTL_CLK_NOC_SHRM_PCLK:
        {
            if((numerator != 1) || (denominator < 1) || (denominator > 8))
                return false;
            else
            {
                /* 1/1, 1/2, 1/3 --- 1/8 */
                ret = sysctl_clk->shrm_clk_cfg;
                ret &= 0xffe3ffff;
                sysctl_clk->shrm_clk_cfg = ret | (((denominator - 1) << 18) | (1 << 31));
                return true;
            }
        }

        case SYSCTL_CLK_NOC_SHRM_MCLK:
        case SYSCTL_CLK_GSDMA_PCLK:
        case SYSCTL_CLK_GSDMA_CLK:
        case SYSCTL_CLK_NOAI_2D_PCLK:
        case SYSCTL_CLK_NOAI_2D_CLK:
        case SYSCTL_CLK_PDMA_PCLK:
        case SYSCTL_CLK_PDMA_CLK:
            return false;

        /*--------------------------- DDR CLOCK ------------------------------------*/
        case SYSCTL_CLK_DDRC_CORE_CLK:
        {
            if((numerator != 1) || (denominator < 1) || (denominator > 16))
                return false;
            else
            {
                /* 1/1, 1/2, 1/3 --- 1/16 */
                ret = sysctl_clk->ddr_clk_cfg;
                ret &= 0xffffc3ff;
                sysctl_clk->ddr_clk_cfg = ret | (((denominator - 1) << 10) | (1 << 31));
                return true;
            }
        }
        case SYSCTL_CLK_DDR_BYPASSPCLK:
            return false;
        case SYSCTL_CLK_NOC_DDRC_PCLK:
        {
            if((numerator != 1) || (denominator < 1) || (denominator > 16))
                return false;
            else
            {
                /* 1/1, 1/2, 1/3 --- 1/16 */
                ret = sysctl_clk->ddr_clk_cfg;
                ret &= 0xfffc3fff;
                sysctl_clk->ddr_clk_cfg = ret | (((denominator - 1) << 14) | (1 << 31));
                return true;
            }
        }

        case SYSCTL_CLK_DDRC_P0_CLK:
        case SYSCTL_CLK_NOC_DDRC_P4_CLK:
        case SYSCTL_CLK_NOC_DDRC_P3_ACLK:
        case SYSCTL_CLK_NOC_DDRC_P2_ACLK:
        case SYSCTL_CLK_NOC_DDRC_P1_ACLK:
            return false;

        /*--------------------------- ISP CLOCK ------------------------------------*/
        case SYSCTL_CLK_ISP_CFG_CLK:
        {
            if((numerator != 1) || (denominator < 1) || (denominator > 32))
                return false;
            else
            {
                /* 1/1, 1/2, 1/3 --- 1/32 */
                ret = sysctl_clk->isp_clkdiv_cfg;
                ret &= 0xffffffe0;
                sysctl_clk->isp_clkdiv_cfg = ret | (((denominator - 1) << 0) | (1 << 31));
                return true;
            }
        }
        case SYSCTL_CLK_CSI0_PIXEL_CLK:
        {
            if((numerator != 1) || (denominator < 1) || (denominator > 64))
                return false;
            else
            {
                /* 1/1, 1/2, 1/3 --- 1/64 */
                ret = sysctl_clk->isp_clkdiv_cfg;
                ret &= 0xfffff81f;
                sysctl_clk->isp_clkdiv_cfg = ret | (((denominator - 1) << 5) | (1 << 31));
                return true;
            }
        }
        case SYSCTL_CLK_CSI1_PIXEL_CLK:
        {
            if((numerator != 1) || (denominator < 1) || (denominator > 64))
                return false;
            else
            {
                /* 1/1, 1/2, 1/3 --- 1/64 */
                ret = sysctl_clk->isp_clkdiv_cfg;
                ret &= 0xfffe07ff;
                sysctl_clk->isp_clkdiv_cfg = ret | (((denominator - 1) << 11) | (1 << 31));
                return true;
            }
        }
        case SYSCTL_CLK_CSI2_PIXEL_CLK:
        {
            if((numerator != 1) || (denominator < 1) || (denominator > 64))
                return false;
            else
            {
                /* 1/1, 1/2, 1/3 --- 1/64 */
                ret = sysctl_clk->isp_clkdiv_cfg;
                ret &= 0xff81ffff;
                sysctl_clk->isp_clkdiv_cfg = ret | (((denominator - 1) << 17) | (1 << 31));
                return true;
            }
        }

        case SYSCTL_CLK_MCLK0:
        {
            if((numerator != 1) || (denominator < 1) || (denominator > 32))
                return false;
            else
            {
                /* 1/1, 1/2, 1/3 --- 1/32 */
                ret = sysctl_clk->mclk_cfg;
                ret &= 0xfffffc1f;
                sysctl_clk->mclk_cfg = ret | (((denominator - 1) << 5) | (1 << 31));
                return true;
            }
        }
        case SYSCTL_CLK_MCLK1:
        {
            if((numerator != 1) || (denominator < 1) || (denominator > 32))
                return false;
            else
            {
                /* 1/1, 1/2, 1/3 --- 1/32 */
                ret = sysctl_clk->mclk_cfg;
                ret &= 0xfffe0fff;
                sysctl_clk->mclk_cfg = ret | (((denominator - 1) << 12) | (1 << 31));
                return true;
            }
        }
        case SYSCTL_CLK_MCLK2:
        {
            if((numerator != 1) || (denominator < 1) || (denominator > 32))
                return false;
            else
            {
                /* 1/1, 1/2, 1/3 --- 1/32 */
                ret = sysctl_clk->mclk_cfg;
                ret &= 0xff07ffff;
                sysctl_clk->mclk_cfg = ret | (((denominator - 1) << 19) | (1 << 31));
                return true;
            }
        }

        case SYSCTL_CLK_ISP_CLK:
        {
            if((numerator != 1) || (denominator < 1) || (denominator > 8))
                return false;
            else
            {
                /* 1/1, 1/2, 1/3 --- 1/8 */
                ret = sysctl_clk->isp_clkdiv_cfg;
                ret &= 0xe3ffffff;
                sysctl_clk->isp_clkdiv_cfg = ret | (((denominator - 1) << 26) | (1 << 31));
                return true;
            }
        }
        case SYSCTL_CLK_ISP_PCLK:
        case SYSCTL_CLK_CSI0_CFG_CLK:
        case SYSCTL_CLK_CSI1_CFG_CLK:
        case SYSCTL_CLK_CSI2_CFG_CLK:
        case SYSCTL_CLK_ISP_DPHY_PCLK:
        case SYSCTL_CLK_CSI0_PCLK:
        case SYSCTL_CLK_CSI1_PCLK:
        case SYSCTL_CLK_CSI2_PCLK:
        case SYSCTL_CLK_ISP_MCM_CLK:
        case SYSCTL_CLK_ISP_DW_HCLK:
        case SYSCTL_CLK_ISP_DW_ACLK:
            return false;

        case SYSCTL_CLK_ISP_HCLK:
        {
            if((numerator != 1) || (denominator < 1) || (denominator > 8))
                return false;
            else
            {
                /* 1/1, 1/2, 1/3 --- 1/8 */
                ret = sysctl_clk->isp_clkdiv_cfg;
                ret &= 0xfc7fffff;
                sysctl_clk->isp_clkdiv_cfg = ret | (((denominator - 1) << 23) | (1 << 31));
                return true;
            }
        }

        case SYSCTL_CLK_ISP_ACLK:

        case SYSCTL_CLK_ISPDWE_CLK:
        case SYSCTL_CLK_ISPVSE_CLK:
            return false;

        /*--------------------------- DPU CLOCK ------------------------------------*/
        case SYSCTL_CLK_DPU_CLK:
        {
            if((numerator > 16) || (numerator < 1) || (denominator != 16))
                return false;
            else
            {
                /* 1/16, 2/16, 3/16 --- 16/16 */
                ret = sysctl_clk->dpu_clk_cfg;
                ret &= 0xffffff87;
                sysctl_clk->dpu_clk_cfg = ret | (((numerator - 1) << 3) | (1 << 31));
                return true;
            }
        }
        case SYSCTL_CLK_DPU_ACLK:
            return false;
        case SYSCTL_CLK_NOC_DPU_PCLK:
        {
            if((numerator != 1) || (denominator < 1) || (denominator > 8))
                return false;
            else
            {
                /* 1/1, 1/2, 1/3 --- 1/8 */
                ret = sysctl_clk->dpu_clk_cfg;
                ret &= 0xfffffc7f;
                sysctl_clk->dpu_clk_cfg = ret | (((denominator - 1) << 7) | (1 << 31));
                return true;
            }
        }

        /*--------------------------- DISPLAY CLOCK ------------------------------------*/
        case SYSCTL_CLK_NOC_DISP_HCLK:
        {
            if((numerator != 1) || (denominator < 1) || (denominator > 8))
                return false;
            else
            {
                /* 1/1, 1/2, 1/3 --- 1/8 */
                ret = sysctl_clk->disp_clk_div;
                ret &= 0xfffffff8;
                sysctl_clk->disp_clk_div = ret | (((denominator - 1) << 0) | (1 << 31));
                return true;
            }
        }
        case SYSCTL_CLK_DISP_ACLK:
        case SYSCTL_CLK_DISP_VO_PCLK:
        case SYSCTL_CLK_DISP_DSI_PCLK:
        case SYSCTL_CLK_DISP_DSI_PHY_PCLK:
        case SYSCTL_CLK_DISP_GPU_HCLK:
        case SYSCTL_CLK_DISP_VO_ACLK:
        case SYSCTL_CLK_DISP_GPU_ACLK:
            return false;

        case SYSCTL_CLK_DISP_CLKEXT:
        {
            if((numerator != 1) || (denominator < 1) || (denominator > 16))
                return false;
            else
            {
                /* 1/1, 1/2, 1/3 --- 1/16 */
                ret = sysctl_clk->disp_clk_div;
                ret &= 0xfff0ffff;
                sysctl_clk->disp_clk_div = ret | (((denominator - 1) << 16) | (1 << 31));
                return true;
            }
        }
        case SYSCTL_CLK_CLK_GPU:
        {
            if((numerator != 1) || (denominator < 1) || (denominator > 16))
                return false;
            else
            {
                /* 1/1, 1/2, 1/3 --- 1/16 */
                ret = sysctl_clk->disp_clk_div;
                ret &= 0xff0fffff;
                sysctl_clk->disp_clk_div = ret | (((denominator - 1) << 20) | (1 << 31));
                return true;
            }
        }

        case SYSCTL_CLK_DPIPCLK:
        {
            if((numerator != 1) || (denominator < 1) || (denominator > 256))
                return false;
            else
            {
                /* 1/1, 1/2, 1/3 --- 1/256 */
                ret = sysctl_clk->disp_clk_div;
                ret &= 0xfffff807;
                sysctl_clk->disp_clk_div = ret | (((denominator - 1) << 3) | (1 << 31));
                return true;
            }
        }
        case SYSCTL_CLK_DISP_CFGCLK:
        {
            if((numerator != 1) || (denominator < 1) || (denominator > 32))
                return false;
            else
            {
                /* 1/1, 1/2, 1/3 --- 1/32 */
                ret = sysctl_clk->disp_clk_div;
                ret &= 0xffff07ff;
                sysctl_clk->disp_clk_div = ret | (((denominator - 1) << 11) | (1 << 31));
                return true;
            }
        }

        case SYSCTL_CLK_DISP_REFCLK:
            return false;

        /*--------------------------- SEC CLOCK ------------------------------------*/
        case SYSCTL_CLK_NOC_SEC_PCLK:
        {
            if((numerator != 1) || (denominator < 1) || (denominator > 8))
                return false;
            else
            {
                /* 1/1, 1/2, 1/3 --- 1/8 */
                ret = sysctl_clk->sec_clk_div;
                ret &= 0xfffffff1;
                sysctl_clk->sec_clk_div = ret | (((denominator - 1) << 1) | (1 << 31));
                return true;
            }
        }
        case SYSCTL_CLK_FIXCLK:
        {
            if((numerator != 1) || (denominator < 1) || (denominator > 32))
                return false;
            else
            {
                /* 1/1, 1/2, 1/3 --- 1/32 */
                ret = sysctl_clk->sec_clk_div;
                ret &= 0xfffff83f;
                sysctl_clk->sec_clk_div = ret | (((denominator - 1) << 6) | (1 << 31));
                return true;
            }
        }

        case SYSCTL_CLK_NOC_SEC_ACLK:
        {
            if((numerator != 1) || (denominator < 1) || (denominator > 8))
                return false;
            else
            {
                /* 1/1, 1/2, 1/3 --- 1/8 */
                ret = sysctl_clk->sec_clk_div;
                ret &= 0xffffc7ff;
                sysctl_clk->sec_clk_div = ret | (((denominator - 1) << 11) | (1 << 31));
                return true;
            }
        }

        /*--------------------------- USB TEST MODE CLOCK ------------------------------------*/
        case SYSCTL_CLK_USB_CLK480:
        {
            if((numerator != 1) || (denominator < 1) || (denominator > 8))
                return false;
            else
            {
                /* 1/1, 1/2, 1/3 --- 1/8 */
                ret = sysctl_clk->usb_test_clk_div;
                ret &= 0xfffffff1;
                sysctl_clk->usb_test_clk_div = ret | (((denominator - 1) << 1) | (1 << 31));
                return true;
            }
        }
        case SYSCTL_CLK_USB_CLK100:
        {
            if((numerator != 1) || (denominator < 1) || (denominator > 8))
                return false;
            else
            {
                /* 1/1, 1/2, 1/3 --- 1/8 */
                ret = sysctl_clk->usb_test_clk_div;
                ret &= 0xffffff8f;
                sysctl_clk->usb_test_clk_div = ret | (((denominator - 1) << 4) | (1 << 31));
                return true;
            }
        }

        /*--------------------------- DPHY DFT MODE CLOCK ------------------------------------*/
        case SYSCTL_CLK_DPHY_TEST_CLK:
        {
            if((numerator != 1) || (denominator < 1) || (denominator > 16))
                return false;
            else
            {
                /* 1/1, 1/2, 1/3 --- 1/16 */
                ret = sysctl_clk->dphy_test_clk_div;
                ret &= 0xffffffe1;
                sysctl_clk->dphy_test_clk_div = ret | (((denominator - 1) << 1) | (1 << 31));
                return true;
            }
        }

        /*--------------------------- SPI2AXI CLOCK ------------------------------------*/
        case SYSCTL_CLK_NOC_SPI2AXI_ACLK:
        {
            if((numerator != 1) || (denominator < 1) || (denominator > 8))
                return false;
            else
            {
                /* 1/1, 1/2, 1/3 --- 1/8 */
                ret = sysctl_clk->spi2axi_clk_div;
                ret &= 0xfffffff1;
                sysctl_clk->spi2axi_clk_div = ret | (((denominator - 1) << 1) | (1 << 31));
                return true;
            }
        }

        default:
            return false;
    }
}

/* 获取本时钟节点的分频系数 */
double sysctl_clk_get_leaf_div(sysctl_clk_node_e leaf)
{
    switch(leaf)
    {
        /*--------------------------- CPU0 CLOCK ------------------------------------*/
        case SYSCTL_CLK_CPU0_CORE_CLK_ROOT:
            return (double)(((sysctl_clk->cpu0_clk_cfg >> 1) & 0xF) + 1) / 16.0;    /* 1/16 --- 16/16 */
        case SYSCTL_CLK_CPU0_PLICLK:
            return 1.0/(double)(((sysctl_clk->cpu0_clk_cfg >> 10) & 0x7) + 1);      /* 1/1, 1/2, 1/3 --- 1/8 */
        case SYSCTL_CLK_CPU0_ACLK:
            return 1.0/(double)(((sysctl_clk->cpu0_clk_cfg >> 6) & 0x7) + 1);      /* 1/1, 1/2, 1/3 --- 1/8 */
        case SYSCTL_CLK_TDI_PCLK:
        case SYSCTL_CLK_CPU0_PCLK:
            return 1.0/(double)(((sysctl_clk->cpu0_clk_cfg >> 15) & 0x7) + 1);      /* 1/1, 1/2, 1/3 --- 1/8 */

        /*--------------------------- CPU1 CLOCK ------------------------------------*/
        case SYSCTL_CLK_CPU1_CORE_CLK_ROOT:
            return 1.0/(double)(((sysctl_clk->cpu1_clk_cfg >> 3) & 0x7) + 1);      /* 1/1, 1/2, 1/3 --- 1/8 */
        case SYSCTL_CLK_CPU1_PLICLK:
            return 1.0/(double)(((sysctl_clk->cpu1_clk_cfg >> 16) & 0x7) + 1);      /* 1/1, 1/2, 1/3 --- 1/8 */
        case SYSCTL_CLK_CPU1_ACLK:
            return 1.0/(double)(((sysctl_clk->cpu1_clk_cfg >> 12) & 0x7) + 1);      /* 1/1, 1/2, 1/3 --- 1/8 */
        case SYSCTL_CLK_CPU1_PCLK:
            return 1;           /* CPU1_CLK_CFG has no divider field. */

        /*--------------------------- AI CLOCK ------------------------------------*/
        case SYSCTL_CLK_AI_CORE_CLK:
            return 1.0/(double)(((sysctl_clk->ai_clk_cfg >> 3) & 0x7) + 1);      /* 1/1, 1/2, 1/3 --- 1/8 */
        case SYSCTL_CLK_AI_ACLK:
            return 1;

        /*--------------------------- VPU CLOCK ------------------------------------*/
        case SYSCTL_CLK_VPU_CLK_ROOT:
            return (double)(((sysctl_clk->vpu_clk_cfg >> 1) & 0xF) + 1) / 16.0;    /* 1/16 --- 16/16 */
        case SYSCTL_CLK_VPU_ACLK_ROOT:
            return 1.0/(double)(((sysctl_clk->vpu_clk_cfg >> 6) & 0xF) + 1);      /* 1/1, 1/2, 1/3 --- 1/16 */
        case SYSCTL_CLK_VPU_ACLK:
            return 1;
        case SYSCTL_CLK_NOC_VPU_CFG_CLK:
            return 1.0/(double)(((sysctl_clk->vpu_clk_cfg >> 11) & 0xF) + 1);      /* 1/1, 1/2, 1/3 --- 1/16 */

        /*--------------------------- PMU CLOCK ------------------------------------*/
        case SYSCTL_CLK_PMU_PCLK:
            return 1;

        /*--------------------------- HS CLOCK ------------------------------------*/
        case SYSCTL_CLK_NOC_HS_OSPI_HCLK:
            return 1.0/(double)(((sysctl_clk->hs_sdclk_cfg >> 0) & 0x7) + 1);      /* 1/1, 1/2, 1/3 --- 1/8 */
        case SYSCTL_CLK_HS_HCLK_HIGH:
            return 1;
        case SYSCTL_CLK_NOC_HS_SUB_HCLK:
            return 1.0/(double)(((sysctl_clk->hs_sdclk_cfg >> 3) & 0x7) + 1);      /* 1/1, 1/2, 1/3 --- 1/8 */
        case SYSCTL_CLK_SD0_HCLK:
        case SYSCTL_CLK_SD1_HCLK:
        case SYSCTL_CLK_USB0_HCLK:
        case SYSCTL_CLK_USB1_HCLK:
        case SYSCTL_CLK_SSI0_HCLK:
        case SYSCTL_CLK_SSI1_HCLK:
        case SYSCTL_CLK_SSI2_HCLK:
            return 1;

        case SYSCTL_CLK_NOC_HS_OSPI_ACLK:
            return 1.0/(double)(((sysctl_clk->hs_spi_cfg >> 9) & 0x7) + 1);      /* 1/1, 1/2, 1/3 --- 1/8 */
        case SYSCTL_CLK_SSI1_CLK:
            return 1.0/(double)(((sysctl_clk->hs_spi_cfg >> 3) & 0x7) + 1);      /* 1/1, 1/2, 1/3 --- 1/8 */
        case SYSCTL_CLK_SSI2_CLK:
            return 1.0/(double)(((sysctl_clk->hs_spi_cfg >> 6) & 0x7) + 1);      /* 1/1, 1/2, 1/3 --- 1/8 */
        case SYSCTL_CLK_NOC_HS_QSPI_ACLK:
            return 1.0/(double)(((sysctl_clk->hs_spi_cfg >> 12) & 0x7) + 1);      /* 1/1, 1/2, 1/3 --- 1/8 */
        case SYSCTL_CLK_SSI1_ACLK:
        case SYSCTL_CLK_SSI2_ACLK:
            return 1;

        case SYSCTL_CLK_SSI0_CLK:
            return 1;

        case SYSCTL_CLK_NOC_HS_SD_ACLK:
            return 1.0/(double)(((sysctl_clk->hs_sdclk_cfg >> 6) & 0x7) + 1);      /* 1/1, 1/2, 1/3 --- 1/8 */
        case SYSCTL_CLK_SD_BCLK:
            return 1.0/(double)(((sysctl_clk->hs_sdclk_cfg >> 9) & 0x7) + 1);      /* 1/1, 1/2, 1/3 --- 1/8 */
        case SYSCTL_CLK_SD0_ACLK:
        case SYSCTL_CLK_SD1_ACLK:
        case SYSCTL_CLK_SD0_BCLK:
        case SYSCTL_CLK_SD1_BCLK:
            return 1;

        case SYSCTL_CLK_SD_CCLK:
            return 1.0/(double)(((sysctl_clk->hs_sdclk_cfg >> 12) & 0x7) + 1);      /* maxinum = 1/2, 1/2, 1/3 --- 1/8 */
        case SYSCTL_CLK_SD0_CCLK_TX:
        case SYSCTL_CLK_SD1_CCLK_TX:
            return 1;

        case SYSCTL_CLK_PLL0_OCLK_DIV16:
            return 1.0/16;
        case SYSCTL_CLK_USB_REFCLK_50M:
            return 1.0/(double)(((sysctl_clk->hs_spi_cfg >> 15) & 0x7) + 1);        /* 1/1, 1/2, 1/3 --- 1/8 */
        case SYSCTL_CLK_USB0_REFCLK:
        case SYSCTL_CLK_USB1_REFCLK:
            return 1;

        case SYSCTL_CLK_SD_TMCLK:
            return 1.0/(double)(((sysctl_clk->hs_sdclk_cfg >> 15) & 0x1F) + 1);      /* maxinum = 1/24, 1/24 --- 1/32 */
        case SYSCTL_CLK_SD0_TMCLK:
        case SYSCTL_CLK_SD1_TMCLK:
            return 1;

        /*--------------------------- LS CLOCK ------------------------------------*/
        case SYSCTL_CLK_NOC_LS_PCLK:
            return 1.0/(double)(((sysctl_clk->ls_clkdiv_cfg >> 0) & 0x7) + 1);      /* 1/1, 1/2, 1/3 --- 1/8 */
        case SYSCTL_CLK_UART0_PCLK:
        case SYSCTL_CLK_UART1_PCLK:
        case SYSCTL_CLK_UART2_PCLK:
        case SYSCTL_CLK_UART3_PCLK:
        case SYSCTL_CLK_UART4_PCLK:
        case SYSCTL_CLK_I2C0_PCLK:
        case SYSCTL_CLK_I2C1_PCLK:
        case SYSCTL_CLK_I2C2_PCLK:
        case SYSCTL_CLK_I2C3_PCLK:
        case SYSCTL_CLK_I2C4_PCLK:
        case SYSCTL_CLK_GPIO_PCLK:
        case SYSCTL_CLK_PWM_PCLK:
        case SYSCTL_CLK_JAMLINK0_PCLK:
        case SYSCTL_CLK_JAMLINK1_PCLK:
        case SYSCTL_CLK_JAMLINK2_PCLK:
        case SYSCTL_CLK_JAMLINK3_PCLK:
        case SYSCTL_CLK_AUDIO_PCLK:
        case SYSCTL_CLK_ADC_PCLK:
        case SYSCTL_CLK_CODEC_PCLK:
            return 1;

        case SYSCTL_CLK_UART0_CLK:
            return 1.0/(double)(((sysctl_clk->uart_i2c_clkdiv_cfg >> 0) & 0x7) + 1);      /* 1/1, 1/2, 1/3 --- 1/8 */
        case SYSCTL_CLK_UART1_CLK:
            return 1.0/(double)(((sysctl_clk->uart_i2c_clkdiv_cfg >> 3) & 0x7) + 1);      /* 1/1, 1/2, 1/3 --- 1/8 */
        case SYSCTL_CLK_UART2_CLK:
            return 1.0/(double)(((sysctl_clk->uart_i2c_clkdiv_cfg >> 6) & 0x7) + 1);      /* 1/1, 1/2, 1/3 --- 1/8 */
        case SYSCTL_CLK_UART3_CLK:
            return 1.0/(double)(((sysctl_clk->uart_i2c_clkdiv_cfg >> 9) & 0x7) + 1);      /* 1/1, 1/2, 1/3 --- 1/8 */
        case SYSCTL_CLK_UART4_CLK:
            return 1.0/(double)(((sysctl_clk->uart_i2c_clkdiv_cfg >> 12) & 0x7) + 1);      /* 1/1, 1/2, 1/3 --- 1/8 */

        case SYSCTL_CLK_JAMLINK0_CO:
        case SYSCTL_CLK_JAMLINK1_CO:
        case SYSCTL_CLK_JAMLINK2_CO:
        case SYSCTL_CLK_JAMLINK3_CO:
            return 1.0/(double)(2 * (((sysctl_clk->ls_clkdiv_cfg >> 23) & 0xFF) + 1));      /* 1/2, 1/4, 1/8 --- 1/512 */

        case SYSCTL_CLK_I2C0_CLK:
            return 1.0/(double)(((sysctl_clk->uart_i2c_clkdiv_cfg >> 15) & 0x7) + 1);      /* 1/1, 1/2, 1/3 --- 1/8 */
        case SYSCTL_CLK_I2C1_CLK:
            return 1.0/(double)(((sysctl_clk->uart_i2c_clkdiv_cfg >> 18) & 0x7) + 1);      /* 1/1, 1/2, 1/3 --- 1/8 */
        case SYSCTL_CLK_I2C2_CLK:
            return 1.0/(double)(((sysctl_clk->uart_i2c_clkdiv_cfg >> 21) & 0x7) + 1);      /* 1/1, 1/2, 1/3 --- 1/8 */
        case SYSCTL_CLK_I2C3_CLK:
            return 1.0/(double)(((sysctl_clk->uart_i2c_clkdiv_cfg >> 24) & 0x7) + 1);      /* 1/1, 1/2, 1/3 --- 1/8 */
        case SYSCTL_CLK_I2C4_CLK:
            return 1.0/(double)(((sysctl_clk->uart_i2c_clkdiv_cfg >> 27) & 0x7) + 1);      /* 1/1, 1/2, 1/3 --- 1/8 */
        case SYSCTL_CLK_CODEC_ADC_MCLK:
            return (double)((sysctl_clk->codec_adc_mclkdiv_cfg >> 14) & 0x1fff)/(double)((sysctl_clk->codec_adc_mclkdiv_cfg >> 0) & 0x3fff);
        case SYSCTL_CLK_CODEC_DAC_MCLK:
            return (double)((sysctl_clk->codec_dac_mclkdiv_cfg >> 14) & 0x1fff)/(double)((sysctl_clk->codec_dac_mclkdiv_cfg >> 0) & 0x3fff);
        case SYSCTL_CLK_AUD_DEV_CLK:
            return (double)((sysctl_clk->audio_clkdiv_cfg >> 16) & 0x7fff)/(double)((sysctl_clk->audio_clkdiv_cfg >> 0) & 0xffff);
        case SYSCTL_CLK_PDM_CLK_ROOT:
            return (double)((sysctl_clk->pdm_clkdiv_cfg1 >> 0) & 0xffff)/(double)((sysctl_clk->pdm_clkdiv_cfg0 >> 0) & 0x1ffff);
        case SYSCTL_CLK_ADC_CLK:
            return 1.0/(double)(((sysctl_clk->ls_clkdiv_cfg >> 3) & 0x3FF) + 1);      /* 1/1, 1/2, 1/3 --- 1/1024 */

        case SYSCTL_CLK_GPIO_DBCLK:
            return 1.0/(double)(((sysctl_clk->ls_clkdiv_cfg >> 13) & 0x3FF) + 1);      /* 1/1, 1/2, 1/3 --- 1/1024 */

        /*--------------------------- SYSCTL CLOCK ------------------------------------*/
        case SYSCTL_CLK_SYSCTL_PCLK:
            // return 1.0/(double)(((sysctl_clk->sysctl_clk_div_cfg >> 0) & 0x7) + 1);      /* 1/1, 1/2, 1/3 --- 1/8 */
            return 1;
        case SYSCTL_CLK_WDT0_PCLK:
        case SYSCTL_CLK_WDT1_PCLK:
        case SYSCTL_CLK_TIMER_PCLK:
        case SYSCTL_CLK_IOMUX_PCLK:
        case SYSCTL_CLK_MAILBOX_PCLK:
            return 1;

        case SYSCTL_CLK_HDI_CLK:
            return 1.0/(double)(((sysctl_clk->sysctl_clk_div_cfg >> 28) & 0x7) + 1);      /* 1/1, 1/2, 1/3 --- 1/8 */

        case SYSCTL_CLK_STC_CLK:
            return 1.0/(double)(((sysctl_clk->sysctl_clk_div_cfg >> 15) & 0x1F) + 1);      /* 1/1, 1/2, 1/3 --- 1/32 */

        case SYSCTL_CLK_TS_CLK:
            return 1.0/(double)(((sysctl_clk->sysctl_clk_div_cfg >> 20) & 0xFF) + 1);      /* 1/1, 1/2, 1/3 --- 1/256 */

        case SYSCTL_CLK_WDT0_CLK:
            return 1.0/(double)(((sysctl_clk->sysctl_clk_div_cfg >> 3) & 0x3F) + 1);      /* 1/1, 1/2, 1/3 --- 1/64 */

        case SYSCTL_CLK_WDT1_CLK:
            return 1.0/(double)(((sysctl_clk->sysctl_clk_div_cfg >> 9) & 0x3F) + 1);      /* 1/1, 1/2, 1/3 --- 1/64 */

        /*--------------------------- TIMER CLOCK ------------------------------------*/
        case SYSCTL_CLK_TIMERX_PULSE_IN:
            return 1;
        case SYSCTL_CLK_TIMER0_CLK:
            return ((sysctl_clk->sysctl_clken_cfg >> 7) & 0x1) ?
                1 : 1.0/(double)(((sysctl_clk->timer_clk_cfg >> 0) & 0x7) + 1);

        case SYSCTL_CLK_TIMER1_CLK:
            return ((sysctl_clk->sysctl_clken_cfg >> 8) & 0x1) ?
                1 : 1.0/(double)(((sysctl_clk->timer_clk_cfg >> 3) & 0x7) + 1);

        case SYSCTL_CLK_TIMER2_CLK:
            return ((sysctl_clk->sysctl_clken_cfg >> 9) & 0x1) ?
                1 : 1.0/(double)(((sysctl_clk->timer_clk_cfg >> 6) & 0x7) + 1);

        case SYSCTL_CLK_TIMER3_CLK:
            return ((sysctl_clk->sysctl_clken_cfg >> 10) & 0x1) ?
                1 : 1.0/(double)(((sysctl_clk->timer_clk_cfg >> 9) & 0x7) + 1);

        case SYSCTL_CLK_TIMER4_CLK:
            return ((sysctl_clk->sysctl_clken_cfg >> 11) & 0x1) ?
                1 : 1.0/(double)(((sysctl_clk->timer_clk_cfg >> 12) & 0x7) + 1);

        case SYSCTL_CLK_TIMER5_CLK:
            return ((sysctl_clk->sysctl_clken_cfg >> 12) & 0x1) ?
                1 : 1.0/(double)(((sysctl_clk->timer_clk_cfg >> 15) & 0x7) + 1);

        /*--------------------------- SHRM CLOCK ------------------------------------*/
        case SYSCTL_CLK_SRAM_CLK:
            return 1;
        case SYSCTL_CLK_SHRM_DIV2:
            return 1.0/2;
        case SYSCTL_CLK_SHRM_AXIM_CLK:
        case SYSCTL_CLK_SHRM_AXIS_CLK:
        case SYSCTL_CLK_DECOMP_PCLK:
        case SYSCTL_CLK_DECOMP_CLK:
            return 1;

        case SYSCTL_CLK_NOC_SHRM_PCLK:
            return 1.0/(double)(((sysctl_clk->shrm_clk_cfg >> 18) & 0x7) + 1);      /* 1/1, 1/2, 1/3 --- 1/8 */

        case SYSCTL_CLK_NOC_SHRM_MCLK:
        case SYSCTL_CLK_GSDMA_PCLK:
        case SYSCTL_CLK_GSDMA_CLK:
        case SYSCTL_CLK_NOAI_2D_PCLK:
        case SYSCTL_CLK_NOAI_2D_CLK:
        case SYSCTL_CLK_PDMA_PCLK:
        case SYSCTL_CLK_PDMA_CLK:
            return 1;

        /*--------------------------- DDR CLOCK ------------------------------------*/
        case SYSCTL_CLK_DDRC_CORE_CLK:
            return 1.0/(double)(((sysctl_clk->ddr_clk_cfg >> 10) & 0xF) + 1);      /* 1/1, 1/2, 1/3 --- 1/16 */
        case SYSCTL_CLK_DDR_BYPASSPCLK:
            return 1;
        case SYSCTL_CLK_NOC_DDRC_PCLK:
            return 1.0/(double)(((sysctl_clk->ddr_clk_cfg >> 14) & 0xF) + 1);      /* 1/1, 1/2, 1/3 --- 1/16 */

        case SYSCTL_CLK_DDRC_P0_CLK:
        case SYSCTL_CLK_NOC_DDRC_P4_CLK:
        case SYSCTL_CLK_NOC_DDRC_P3_ACLK:
        case SYSCTL_CLK_NOC_DDRC_P2_ACLK:
        case SYSCTL_CLK_NOC_DDRC_P1_ACLK:
            return 1;

        /*--------------------------- ISP CLOCK ------------------------------------*/
        case SYSCTL_CLK_ISP_CFG_CLK:
            return 1.0/(double)(((sysctl_clk->isp_clkdiv_cfg >> 0) & 0x1F) + 1);      /* 1/1, 1/2, 1/3 --- 1/32 */
        case SYSCTL_CLK_ISP_PCLK:
        case SYSCTL_CLK_CSI0_CFG_CLK:
        case SYSCTL_CLK_CSI1_CFG_CLK:
        case SYSCTL_CLK_CSI2_CFG_CLK:
        case SYSCTL_CLK_ISP_DPHY_PCLK:
        case SYSCTL_CLK_CSI0_PCLK:
        case SYSCTL_CLK_CSI1_PCLK:
        case SYSCTL_CLK_CSI2_PCLK:
            return 1;
        case SYSCTL_CLK_CSI0_PIXEL_CLK:
            return 1.0/(double)(((sysctl_clk->isp_clkdiv_cfg >> 5) & 0x3F) + 1);      /* 1/1, 1/2, 1/3 --- 1/64 */
        case SYSCTL_CLK_CSI1_PIXEL_CLK:
            return 1.0/(double)(((sysctl_clk->isp_clkdiv_cfg >> 11) & 0x3F) + 1);      /* 1/1, 1/2, 1/3 --- 1/64 */
        case SYSCTL_CLK_CSI2_PIXEL_CLK:
            return 1.0/(double)(((sysctl_clk->isp_clkdiv_cfg >> 17) & 0x3F) + 1);      /* 1/1, 1/2, 1/3 --- 1/64 */

        case SYSCTL_CLK_MCLK0:
            return 1.0/(double)(((sysctl_clk->mclk_cfg >> 5) & 0x1F) + 1);      /* 1/1, 1/2, 1/3 --- 1/32 */
        case SYSCTL_CLK_MCLK1:
            return 1.0/(double)(((sysctl_clk->mclk_cfg >> 12) & 0x1F) + 1);      /* 1/1, 1/2, 1/3 --- 1/32 */
        case SYSCTL_CLK_MCLK2:
            return 1.0/(double)(((sysctl_clk->mclk_cfg >> 19) & 0x1F) + 1);      /* 1/1, 1/2, 1/3 --- 1/32 */

        case SYSCTL_CLK_ISP_CLK:
            return 1.0/(double)(((sysctl_clk->isp_clkdiv_cfg >> 26) & 0x7) + 1);      /* 1/1, 1/2, 1/3 --- 1/8 */
        case SYSCTL_CLK_ISP_MCM_CLK:
            return 1;

        case SYSCTL_CLK_ISP_HCLK:
            return 1.0/(double)(((sysctl_clk->isp_clkdiv_cfg >> 23) & 0x7) + 1);      /* 1/1, 1/2, 1/3 --- 1/8 */

        case SYSCTL_CLK_ISP_ACLK:

        case SYSCTL_CLK_ISPDWE_CLK:
        case SYSCTL_CLK_ISPVSE_CLK:
        case SYSCTL_CLK_ISP_DW_HCLK:
        case SYSCTL_CLK_ISP_DW_ACLK:
            return 1;

        /*--------------------------- DPU CLOCK ------------------------------------*/
        case SYSCTL_CLK_DPU_CLK:
            return (double)(((sysctl_clk->dpu_clk_cfg >> 3) & 0xF) + 1) / 16.0;    /* 1/16 --- 16/16 */
        case SYSCTL_CLK_DPU_ACLK:
            return 1;
        case SYSCTL_CLK_NOC_DPU_PCLK:
            return 1.0/(double)(((sysctl_clk->dpu_clk_cfg >> 7) & 0x7) + 1);      /* 1/1, 1/2, 1/3 --- 1/8 */

        /*--------------------------- DISPLAY CLOCK ------------------------------------*/
        case SYSCTL_CLK_NOC_DISP_HCLK:
            return 1.0/(double)(((sysctl_clk->disp_clk_div >> 0) & 0x7) + 1);      /* 1/1, 1/2, 1/3 --- 1/8 */
        case SYSCTL_CLK_DISP_ACLK:
        case SYSCTL_CLK_DISP_VO_PCLK:
        case SYSCTL_CLK_DISP_DSI_PCLK:
        case SYSCTL_CLK_DISP_DSI_PHY_PCLK:
        case SYSCTL_CLK_DISP_GPU_HCLK:
        case SYSCTL_CLK_DISP_VO_ACLK:
        case SYSCTL_CLK_DISP_GPU_ACLK:
            return 1;

        case SYSCTL_CLK_DISP_CLKEXT:
            return 1.0/(double)(((sysctl_clk->disp_clk_div >> 16) & 0xF) + 1);      /* 1/1, 1/2, 1/3 --- 1/16 */
        case SYSCTL_CLK_CLK_GPU:
            return 1.0/(double)(((sysctl_clk->disp_clk_div >> 20) & 0xF) + 1);      /* 1/1, 1/2, 1/3 --- 1/16 */

        case SYSCTL_CLK_DPIPCLK:
            return 1.0/(double)(((sysctl_clk->disp_clk_div >> 3) & 0xFF) + 1);      /* 1/1, 1/2, 1/3 --- 1/256 */
        case SYSCTL_CLK_DISP_CFGCLK:
            return 1.0/(double)(((sysctl_clk->disp_clk_div >> 11) & 0x1F) + 1);      /* 1/1, 1/2, 1/3 --- 1/32 */

        case SYSCTL_CLK_DISP_REFCLK:
            return 1;

        /*--------------------------- SEC CLOCK ------------------------------------*/
        case SYSCTL_CLK_NOC_SEC_PCLK:
            return 1.0/(double)(((sysctl_clk->sec_clk_div >> 1) & 0x7) + 1);      /* 1/1, 1/2, 1/3 --- 1/8 */
        case SYSCTL_CLK_FIXCLK:
            return 1.0/(double)(((sysctl_clk->sec_clk_div >> 6) & 0x7) + 1);      /* 1/1, 1/2, 1/3 --- 1/8 */

        case SYSCTL_CLK_NOC_SEC_ACLK:
            return 1.0/(double)(((sysctl_clk->sec_clk_div >> 11) & 0x7) + 1);      /* 1/1, 1/2, 1/3 --- 1/8 */

        /*--------------------------- USB TEST MODE CLOCK ------------------------------------*/
        case SYSCTL_CLK_USB_CLK480:
            return 1.0/(double)(((sysctl_clk->usb_test_clk_div >> 1) & 0x7) + 1);      /* 1/1, 1/2, 1/3 --- 1/8 */
        case SYSCTL_CLK_USB_CLK100:
            return 1.0/(double)(((sysctl_clk->usb_test_clk_div >> 4) & 0x7) + 1);      /* 1/1, 1/2, 1/3 --- 1/8 */

        /*--------------------------- DPHY DFT MODE CLOCK ------------------------------------*/
        case SYSCTL_CLK_DPHY_TEST_CLK:
            return 1.0/(double)(((sysctl_clk->dphy_test_clk_div >> 1) & 0xF) + 1);      /* 1/1, 1/2, 1/3 --- 1/16 */

        /*--------------------------- SPI2AXI CLOCK ------------------------------------*/
        case SYSCTL_CLK_NOC_SPI2AXI_ACLK:
            return 1.0/(double)(((sysctl_clk->spi2axi_clk_div >> 1) & 0x7) + 1);      /* 1/1, 1/2, 1/3 --- 1/8 */

        default:
            return 1;
    }
}

/* 计算本时钟节点的频率。它会搜索整个时钟路径，从时钟源开始计算每一级的分频，最终得出当前时钟节点的频率
*  1. 对于根节点的clock，利用sysctl_boot_get_root_clk_freq(node)获取clock的频率（此时分频系数div=1）；
*  2. 对于叶子节点的clock，利用while循环搜索叶子节点的根节点并计算出整条路径上的div，找到根节点后，利用
*     sysctl_boot_get_root_clk_freq(node) * div计算出叶子节点的频率。
 */
uint32_t sysctl_clk_get_leaf_freq(sysctl_clk_node_e leaf)
{
    double div = 1.0;
    sysctl_clk_node_e node;

    node = leaf;

    if(node == SYSCTL_CLK_TIMERX_PULSE_IN)
    {
        return (uint32_t)(50000000 * div);
    }

    /* calc leaf chain div */
    while(node > SYSCTL_CLK_ROOT_MAX)
    {
        div *= sysctl_clk_get_leaf_div(node);
        node = sysctl_clk_get_leaf_parent(node);
    }

    /* get root freq and calc leaf freq */
    return (uint32_t)(sysctl_boot_get_root_clk_freq(node) * div);
}

void sysctl_clk_set_disp_gpu_en(bool enable)
{
    rt_base_t level = rt_hw_interrupt_disable();

    if (enable)
        sysctl_clk->vo_clk_cfg |= (1U << 6);
    else
        sysctl_clk->vo_clk_cfg &= ~(1U << 6);

    rt_hw_interrupt_enable(level);
}


/* Media clock compatibility APIs live here so CMU has one owner. */
int k230_pm_get(enum k230_pm_dev_id dev);
int k230_pm_put(enum k230_pm_dev_id dev);

static const sysctl_media_clk_node_e media_clk_nodes[] = {
    SYSCTL_CLK_CPU1_CORE_CLK_SRC,
    SYSCTL_CLK_CPU1_CORE_CLK_ROOT,
    SYSCTL_CLK_CPU1_CORE_CLK,
    SYSCTL_CLK_CPU1_PLICLK,
    SYSCTL_CLK_CPU1_ACLK,
    SYSCTL_CLK_CPU1_PCLK,
    SYSCTL_CLK_AI_CORE_CLK,
    SYSCTL_CLK_AI_ACLK,
    SYSCTL_CLK_VPU_CLK_ROOT,
    SYSCTL_CLK_VPU_CLK,
    SYSCTL_CLK_VPU_ACLK_ROOT,
    SYSCTL_CLK_VPU_ACLK,
    SYSCTL_CLK_VPU_CFG_CLK,
    SYSCTL_CLK_AUDIO_PCLK,
    SYSCTL_CLK_CODEC_PCLK,
    SYSCTL_CLK_CODEC_ADC_MCLK,
    SYSCTL_CLK_CODEC_DAC_MCLK,
    SYSCTL_CLK_AUD_DEV_CLK,
    SYSCTL_CLK_PDM_CLK_ROOT,
    SYSCTL_CLK_GSDMA_CLK,
    SYSCTL_CLK_PDMA_CLK,
    SYSCTL_CLK_DDRC_CORE_CLK,
    SYSCTL_CLK_DDR_BYPASSPCLK,
    SYSCTL_CLK_DDRC_PCLK,
    SYSCTL_CLK_DDRC_P0_CLK,
    SYSCTL_CLK_NOC_DDRC_P3_ACLK,
    SYSCTL_CLK_NOC_DDRC_P2_ACLK,
    SYSCTL_CLK_NOC_DDRC_P1_ACLK,
    SYSCTL_CLK_ISP_CFG_CLK,
    SYSCTL_CLK_CSI0_PIXEL_CLK,
    SYSCTL_CLK_CSI1_PIXEL_CLK,
    SYSCTL_CLK_CSI2_PIXEL_CLK,
    SYSCTL_CLK_ISP_CLK,
    SYSCTL_CLK_ISP_MCM_CLK,
    SYSCTL_CLK_ISP_HCLK,
    SYSCTL_CLK_ISP_ACLK,
    SYSCTL_CLK_ISPDWE_CLK,
    SYSCTL_CLK_ISPVSE_CLK,
    SYSCTL_CLK_MCLK0,
    SYSCTL_CLK_MCLK1,
    SYSCTL_CLK_MCLK2,
    SYSCTL_CLK_DPU_CLK,
    SYSCTL_CLK_DPU_ACLK,
    SYSCTL_CLK_DPU_PCLK,
    SYSCTL_CLK_DISP_HCLK,
    SYSCTL_CLK_DISP_ACLK,
    SYSCTL_CLK_DISP_CLKEXT,
    SYSCTL_CLK_CLK_GPU,
    SYSCTL_CLK_DPIPCLK,
    SYSCTL_CLK_DISP_CFGCLK,
    SYSCTL_CLK_DISP_REFCLK,
};

bool sysctl_media_clk_is_node(sysctl_media_clk_node_e node)
{
    for (unsigned int i = 0; i < sizeof(media_clk_nodes) / sizeof(media_clk_nodes[0]); i++) {
        if (media_clk_nodes[i] == node)
            return true;
    }

    return false;
}

static const sysctl_media_clk_node_e media_clk_lock_nodes[] = {
    SYSCTL_CLK_CPU1_CORE_CLK_SRC,
    SYSCTL_CLK_CPU1_CORE_CLK_ROOT,
    SYSCTL_CLK_AI_CORE_CLK,
    SYSCTL_CLK_VPU_CLK_ROOT,
    SYSCTL_CLK_VPU_CLK,
    SYSCTL_CLK_AUDIO_PCLK,
    SYSCTL_CLK_CODEC_PCLK,
    SYSCTL_CLK_CODEC_ADC_MCLK,
    SYSCTL_CLK_CODEC_DAC_MCLK,
    SYSCTL_CLK_AUD_DEV_CLK,
    SYSCTL_CLK_PDM_CLK_ROOT,
    SYSCTL_CLK_MCLK0,
    SYSCTL_CLK_MCLK1,
    SYSCTL_CLK_MCLK2,
    SYSCTL_CLK_DPU_CLK,
    SYSCTL_CLK_CLK_GPU,
};

static struct rt_mutex media_clk_locks[sizeof(media_clk_lock_nodes) / sizeof(media_clk_lock_nodes[0])];
static bool media_clk_locks_ready;

static int media_clk_lock_index(sysctl_media_clk_node_e leaf)
{
    for (unsigned int i = 0; i < sizeof(media_clk_lock_nodes) / sizeof(media_clk_lock_nodes[0]); i++) {
        if (media_clk_lock_nodes[i] == leaf)
            return (int)i;
    }

    return -1;
}

static int media_clk_lock_take(sysctl_media_clk_node_e leaf)
{
    int idx = media_clk_lock_index(leaf);

    if (idx < 0 || !media_clk_locks_ready || rt_interrupt_get_nest() != 0U)
        return -1;

    rt_mutex_take(&media_clk_locks[idx], RT_WAITING_FOREVER);
    return idx;
}

static void media_clk_lock_release(int idx)
{
    if (idx >= 0)
        rt_mutex_release(&media_clk_locks[idx]);
}

/* 获取时钟树上叶子节点的时钟源，即读取MUX对应的寄存器的值；默认返回SYSCTL_CLK_ROOT_MAX */
/* 设置时钟节点enable,注意:只设置本时钟节点的enable，不会设置上游时钟的enable。*/
/* 获取本时钟节点的enable状态 */
/* 设置本时钟节点的分频系数, freq = root_freq * numerator / denominator */
/* 获取本时钟节点的分频系数 */
/* 计算本时钟节点的频率。它会搜索整个时钟路径，从时钟源开始计算每一级的分频，最终得出当前时钟节点的频率
*  1. 对于根节点的clock，利用sysctl_boot_get_root_clk_freq(node)获取clock的频率（此时分频系数div=1）；
*  2. 对于叶子节点的clock，利用while循环搜索叶子节点的根节点并计算出整条路径上的div，找到根节点后，利用
*     sysctl_boot_get_root_clk_freq(node) * div计算出叶子节点的频率。
 */
uint32_t sysctl_media_clk_get_leaf_freq(sysctl_media_clk_node_e leaf)
{
    return sysctl_clk_get_leaf_freq(leaf);
}


/**
 * 实际上，本函数主要针对小数分频。
 *
 * method=1/2，本函数会根据父节点时钟、实际需要的时钟，计算分频系数；
 * method=3，本函数会根据输入节点、实际需要的时钟，计算分频系数
*/
int sysctl_media_clk_find_approximate(sysctl_media_clk_node_e leaf,
                                     uint32_t mul_min,
                                     uint32_t mul_max,
                                     uint32_t div_min,
                                     uint32_t div_max,
                                     sysctl_media_clk_mul_div_methord_e method,
                                     unsigned long rate,
                                     unsigned long parent_rate,
                                     uint32_t *div,
                                     uint32_t *mul)
{
    long abs_min;
    long abs_current;
    /* we used interger to instead of float */
    long perfect_divide;

    uint32_t a,b;
    int i = 0;
    int n = 0;
    unsigned long mul_ulong,div_ulong;

    volatile uint32_t codec_clk[9] = {
        2048000,
        3072000,
        4096000,
        6144000,
        8192000,
        11289600,
        12288000,
        24576000,
        49152000
    };
    volatile uint32_t codec_div[9][2] = {
        {3125, 16},
        {3125, 24},
        {3125, 32},
        {3125, 48},
        {3125, 64},
        {15625, 441},
        {3125, 96},
        {3125, 192},
        {3125, 384}
    };

    volatile uint32_t pdm_clk[20] = {
        128000,
        192000,
        256000,
        384000,
        512000,
        768000,
        1024000,
        1411200,
        1536000,
        2048000,
        2822400,
        3072000,
        4096000,
        5644800,
        6144000,
        8192000,
        11289600,
        12288000,
        24576000,
        49152000
    };
    volatile uint32_t pdm_div[20][2] = {
        {3125, 1},
        {6250, 3},
        {3125, 2},
        {3125, 3},
        {3125, 4},
        {3125, 6},
        {3125, 8},
        {125000, 441},
        {3125, 12},
        {3125, 16},
        {62500, 441},
        {3125, 24},
        {3125, 32},
        {31250, 441},
        {3125, 48},
        {3125, 64},
        {15625, 441},
        {3125, 96},
        {3125, 192},
        {3125, 384}
    };

    switch(method) {

        /* only mul can be changeable, 1/8,2/8,3/8...*/
        case SYSCTL_CLK_MUL_CHANGEABLE: {
            perfect_divide = (long)((parent_rate*1000) / rate);
            abs_min = abs(perfect_divide - (long)(((long)div_max*1000)/(long)mul_min));

            for(i = mul_min + 1; i <= mul_max; i++) {
                abs_current = abs(perfect_divide - (long)((long)((long)div_max*1000)/(long)i));
                if(abs_min > abs_current ) {
                    abs_min = abs_current;
                    *mul = i;
                }
            }

            *div = div_min;
            break;
        }
        /* only div can be changeable, 1/1,1/2,1/3...*/
        case SYSCTL_CLK_DIV_CHANGEABLE: {
            perfect_divide = (long)((parent_rate*1000) / rate);
            abs_min = abs(perfect_divide - (long)(((long)div_min*1000)/(long)mul_max));
            *div = div_min;

            for(i = div_min + 1; i <= div_max; i++) {
                abs_current = abs(perfect_divide - (long)((long)((long)i*1000)/(long)mul_max));
                if(abs_min > abs_current ) {
                    abs_min = abs_current;
                    *div = i;
                }
            }
            *mul = mul_min;
            break;
        }

        case SYSCTL_CLK_MUL_DIV_CHANGEABLE: {
            if((SYSCTL_CLK_CODEC_ADC_MCLK == leaf) || (SYSCTL_CLK_CODEC_DAC_MCLK == leaf))
            {
                for(int i=0;i<9;i++)
                {
                    if(0 == (rate - codec_clk[i]))
                    {
                        *div = codec_div[i][0];
                        *mul = codec_div[i][1];
                    }
                }
            }
            else if((SYSCTL_CLK_AUD_DEV_CLK == leaf) || (SYSCTL_CLK_PDM_CLK_ROOT == leaf))
            {
                for(int i=0;i<20;i++)
                {
                    if(0 == (rate - pdm_clk[i]))
                    {
                        *div = pdm_div[i][0];
                        *mul = pdm_div[i][1];
                    }
                }
            }
            else
            {
                return -1;
            }
            break;
        }

        default:{
            return -3;
        }
    }
    return 0;
}

bool sysctl_media_clk_set_leaf_parent_div_raw(sysctl_media_clk_node_e leaf, sysctl_media_clk_node_e parent, uint32_t numerator, uint32_t denominator)
{
    uint32_t reg, value;

    if (denominator == 0)
        return false;

    switch (leaf) {
    /*--------------------------- CPU1 CLOCK ------------------------------------*/
    case SYSCTL_CLK_CPU1_CORE_CLK_ROOT:
        if ((numerator != 1) || (denominator < 1) || (denominator > 8))
            return false;

        /* CPU1_CLK_CFG[2:1]: 0=PLL1/2, 1=PLL3, 2=PLL0. */
        if (SYSCTL_CLK_PLL1_OCLK_DIV2 == parent)
            value = 0;
        else if (SYSCTL_CLK_PLL3_OCLK == parent)
            value = 1;
        else if (SYSCTL_CLK_PLL0_OCLK == parent)
            value = 2;
        else
            return false;

        reg = sysctl_clk->cpu1_clk_cfg;
        reg |= ((1 << 31) | (7 << 3));
        sysctl_clk->cpu1_clk_cfg = reg;

        reg &= ~(3 << 1);
        reg |= (value << 1);
        sysctl_clk->cpu1_clk_cfg = reg;

        reg &= ~(7 << 3);
        reg |= ((1 << 31) | ((denominator - 1) << 3));
        sysctl_clk->cpu1_clk_cfg = reg;
        break;
    /*--------------------------- AI CLOCK ------------------------------------*/
    case SYSCTL_CLK_AI_CORE_CLK:
        if ((numerator != 1) || (denominator < 1) || (denominator > 8))
            return false;

        /* AI_CLK_CFG[2]: 0=PLL1/2, 1=PLL3/2. */
        if (SYSCTL_CLK_PLL1_OCLK_DIV2 == parent)
            value = 0;
        else if (SYSCTL_CLK_PLL3_OCLK_DIV2 == parent)
            value = 1;
        else
            return false;

        reg = sysctl_clk->ai_clk_cfg;
        reg |= ((1 << 31) | (7 << 3));
        sysctl_clk->ai_clk_cfg = reg;

        reg &= ~(1 << 2);
        reg |= (value << 2);
        sysctl_clk->ai_clk_cfg = reg;

        reg &= ~(7 << 3);
        reg |= ((1 << 31) | ((denominator - 1) << 3));
        sysctl_clk->ai_clk_cfg = reg;
        break;
    default:
        return false;
    }
    return true;
}


sysctl_media_clk_node_e sysctl_media_clk_get_leaf_parent(sysctl_media_clk_node_e leaf)
{
    return sysctl_clk_get_leaf_parent(leaf);
}

bool sysctl_media_clk_get_leaf_en(sysctl_media_clk_node_e leaf)
{
    return sysctl_clk_get_leaf_en(leaf);
}

double sysctl_media_clk_get_leaf_div(sysctl_media_clk_node_e leaf)
{
    return sysctl_clk_get_leaf_div(leaf);
}

bool sysctl_media_clk_set_leaf_parent(sysctl_media_clk_node_e leaf, sysctl_media_clk_node_e parent)
{
    int lock_idx = media_clk_lock_take(leaf);
    bool ret = sysctl_clk_set_leaf_parent(leaf, parent);

    media_clk_lock_release(lock_idx);
    return ret;
}

void sysctl_media_clk_set_leaf_en(sysctl_media_clk_node_e leaf, bool enable)
{
    int lock_idx = media_clk_lock_take(leaf);

    sysctl_clk_set_leaf_en(leaf, enable);
    media_clk_lock_release(lock_idx);
}

bool sysctl_media_clk_set_leaf_div(sysctl_media_clk_node_e leaf, uint32_t numerator, uint32_t denominator)
{
    int lock_idx = media_clk_lock_take(leaf);
    bool ret = sysctl_clk_set_leaf_div(leaf, numerator, denominator);

    media_clk_lock_release(lock_idx);
    return ret;
}

bool sysctl_media_clk_set_leaf_parent_div(sysctl_media_clk_node_e leaf, sysctl_media_clk_node_e parent, uint32_t numerator, uint32_t denominator)
{
    int lock_idx = media_clk_lock_take(leaf);
    bool ret = sysctl_media_clk_set_leaf_parent_div_raw(leaf, parent, numerator, denominator);

    media_clk_lock_release(lock_idx);
    return ret;
}

struct media_pm_leaf {
    sysctl_media_clk_node_e leaf;
    enum k230_pm_dev_id dev;
};

static const struct media_pm_leaf media_leaf_list[] = {
    {SYSCTL_CLK_AI_CORE_CLK, K230_PM_DEV_AI},
    {SYSCTL_CLK_VPU_CLK, K230_PM_DEV_VPU},
    {SYSCTL_CLK_AUDIO_PCLK, K230_PM_DEV_AUDIO},
    {SYSCTL_CLK_CODEC_PCLK, K230_PM_DEV_AUDIO_CODEC},
    {SYSCTL_CLK_CODEC_ADC_MCLK, K230_PM_DEV_ID_MAX},
    {SYSCTL_CLK_CODEC_DAC_MCLK, K230_PM_DEV_ID_MAX},
    {SYSCTL_CLK_AUD_DEV_CLK, K230_PM_DEV_ID_MAX},
    {SYSCTL_CLK_PDM_CLK_ROOT, K230_PM_DEV_ID_MAX},
    {SYSCTL_CLK_MCLK0, K230_PM_DEV_ID_MAX},
    {SYSCTL_CLK_MCLK1, K230_PM_DEV_ID_MAX},
    {SYSCTL_CLK_MCLK2, K230_PM_DEV_ID_MAX},
    {SYSCTL_CLK_DPU_CLK, K230_PM_DEV_DPU},
    {SYSCTL_CLK_CLK_GPU, K230_PM_DEV_ID_MAX},
};

static uint32_t media_leaf_ref_count[sizeof(media_leaf_list) / sizeof(media_leaf_list[0])];

static int media_leaf_index(sysctl_media_clk_node_e leaf)
{
    for (unsigned int i = 0; i < sizeof(media_leaf_list) / sizeof(media_leaf_list[0]); i++) {
        if (media_leaf_list[i].leaf == leaf)
            return (int)i;
    }

    return -1;
}

int sysctl_media_clk_get_leaf(sysctl_media_clk_node_e leaf)
{
    rt_base_t level;
    bool do_transition;
    int idx = media_leaf_index(leaf);
    int lock_idx = media_clk_lock_take(leaf);
    int ret = 0;

    if (idx < 0) {
        sysctl_clk_set_leaf_en(leaf, true);
        media_clk_lock_release(lock_idx);
        return 0;
    }

    level = rt_hw_interrupt_disable();
    if (media_leaf_ref_count[idx] == UINT32_MAX) {
        rt_hw_interrupt_enable(level);
        rt_kprintf("error: media clk %d enable too many times\n", leaf);
        media_clk_lock_release(lock_idx);
        return -1;
    }
    do_transition = (media_leaf_ref_count[idx] == 0);
    media_leaf_ref_count[idx]++;
    rt_hw_interrupt_enable(level);

    if (!do_transition) {
        media_clk_lock_release(lock_idx);
        return 0;
    }

    if (media_leaf_list[idx].dev != K230_PM_DEV_ID_MAX)
        ret = k230_pm_get(media_leaf_list[idx].dev);
    else
        sysctl_clk_set_leaf_en(leaf, true);

    if (ret == 0) {
        media_clk_lock_release(lock_idx);
        return 0;
    }

    level = rt_hw_interrupt_disable();
    if (media_leaf_ref_count[idx] > 0)
        media_leaf_ref_count[idx]--;
    rt_hw_interrupt_enable(level);
    media_clk_lock_release(lock_idx);
    return ret;
}

int sysctl_media_clk_put_leaf(sysctl_media_clk_node_e leaf)
{
    rt_base_t level;
    bool do_transition;
    int idx = media_leaf_index(leaf);
    int lock_idx = media_clk_lock_take(leaf);
    int ret = 0;

    if (idx < 0) {
        sysctl_clk_set_leaf_en(leaf, false);
        media_clk_lock_release(lock_idx);
        return 0;
    }

    level = rt_hw_interrupt_disable();
    if (media_leaf_ref_count[idx] == 0) {
        rt_hw_interrupt_enable(level);
        media_clk_lock_release(lock_idx);
        return 0;
    }
    media_leaf_ref_count[idx]--;
    do_transition = (media_leaf_ref_count[idx] == 0);
    rt_hw_interrupt_enable(level);

    if (!do_transition) {
        media_clk_lock_release(lock_idx);
        return 0;
    }

    if (media_leaf_list[idx].dev != K230_PM_DEV_ID_MAX)
        ret = k230_pm_put(media_leaf_list[idx].dev);
    else
        sysctl_clk_set_leaf_en(leaf, false);

    if (ret != 0) {
        level = rt_hw_interrupt_disable();
        media_leaf_ref_count[idx]++;
        rt_hw_interrupt_enable(level);
    }

    media_clk_lock_release(lock_idx);
    return ret;
}

void sysctl_media_clk_set_leaf_en_multi(sysctl_media_clk_node_e leaf, bool enable)
{
    if (enable)
        (void)sysctl_media_clk_get_leaf(leaf);
    else
        (void)sysctl_media_clk_put_leaf(leaf);
}


static const char *const media_clk_names[SYSCTL_CLK_NODE_MAX] = {
    [SYSCTL_CLK_OSC24M] = "SYSCTL_CLK_OSC24M",
    [SYSCTL_CLK_ROOT_TIMERX_PULSE_IN] = "SYSCTL_CLK_ROOT_TIMERX_PULSE_IN",
    [SYSCTL_CLK_PLL0_OCLK] = "SYSCTL_CLK_PLL0_OCLK",
    [SYSCTL_CLK_PLL0_OCLK_DIV2] = "SYSCTL_CLK_PLL0_OCLK_DIV2",
    [SYSCTL_CLK_PLL0_OCLK_DIV3] = "SYSCTL_CLK_PLL0_OCLK_DIV3",
    [SYSCTL_CLK_PLL0_OCLK_DIV4] = "SYSCTL_CLK_PLL0_OCLK_DIV4",
    [SYSCTL_CLK_PLL1_OCLK] = "SYSCTL_CLK_PLL1_OCLK",
    [SYSCTL_CLK_PLL1_OCLK_DIV2] = "SYSCTL_CLK_PLL1_OCLK_DIV2",
    [SYSCTL_CLK_PLL1_OCLK_DIV3] = "SYSCTL_CLK_PLL1_OCLK_DIV3",
    [SYSCTL_CLK_PLL1_OCLK_DIV4] = "SYSCTL_CLK_PLL1_OCLK_DIV4",
    [SYSCTL_CLK_PLL2_OCLK] = "SYSCTL_CLK_PLL2_OCLK",
    [SYSCTL_CLK_PLL2_OCLK_DIV2] = "SYSCTL_CLK_PLL2_OCLK_DIV2",
    [SYSCTL_CLK_PLL2_OCLK_DIV3] = "SYSCTL_CLK_PLL2_OCLK_DIV3",
    [SYSCTL_CLK_PLL2_OCLK_DIV4] = "SYSCTL_CLK_PLL2_OCLK_DIV4",
    [SYSCTL_CLK_PLL3_OCLK] = "SYSCTL_CLK_PLL3_OCLK",
    [SYSCTL_CLK_PLL3_OCLK_DIV2] = "SYSCTL_CLK_PLL3_OCLK_DIV2",
    [SYSCTL_CLK_PLL3_OCLK_DIV3] = "SYSCTL_CLK_PLL3_OCLK_DIV3",
    [SYSCTL_CLK_PLL3_OCLK_DIV4] = "SYSCTL_CLK_PLL3_OCLK_DIV4",
    [SYSCTL_CLK_CPU0_CORE_CLK_ROOT] = "SYSCTL_CLK_CPU0_CORE_CLK_ROOT",
    [SYSCTL_CLK_CPU0_PLICLK] = "SYSCTL_CLK_CPU0_PLICLK",
    [SYSCTL_CLK_CPU0_ACLK] = "SYSCTL_CLK_CPU0_ACLK",
    [SYSCTL_CLK_TDI_PCLK] = "SYSCTL_CLK_TDI_PCLK",
    [SYSCTL_CLK_CPU0_PCLK] = "SYSCTL_CLK_CPU0_PCLK",
    [SYSCTL_CLK_CPU1_CORE_CLK_SRC] = "SYSCTL_CLK_CPU1_CORE_CLK_SRC",
    [SYSCTL_CLK_CPU1_PLICLK] = "SYSCTL_CLK_CPU1_PLICLK",
    [SYSCTL_CLK_CPU1_ACLK] = "SYSCTL_CLK_CPU1_ACLK",
    [SYSCTL_CLK_CPU1_PCLK] = "SYSCTL_CLK_CPU1_PCLK",
    [SYSCTL_CLK_AI_CORE_CLK] = "SYSCTL_CLK_AI_CORE_CLK",
    [SYSCTL_CLK_AI_ACLK] = "SYSCTL_CLK_AI_ACLK",
    [SYSCTL_CLK_VPU_CLK] = "SYSCTL_CLK_VPU_CLK",
    [SYSCTL_CLK_VPU_ACLK_ROOT] = "SYSCTL_CLK_VPU_ACLK_ROOT",
    [SYSCTL_CLK_VPU_ACLK] = "SYSCTL_CLK_VPU_ACLK",
    [SYSCTL_CLK_VPU_CFG_CLK] = "SYSCTL_CLK_VPU_CFG_CLK",
    [SYSCTL_CLK_PMU_PCLK] = "SYSCTL_CLK_PMU_PCLK",
    [SYSCTL_CLK_NOC_HS_OSPI_HCLK] = "SYSCTL_CLK_NOC_HS_OSPI_HCLK",
    [SYSCTL_CLK_HS_HCLK_HIGH] = "SYSCTL_CLK_HS_HCLK_HIGH",
    [SYSCTL_CLK_HS_HCLK] = "SYSCTL_CLK_HS_HCLK",
    [SYSCTL_CLK_SD0_HCLK] = "SYSCTL_CLK_SD0_HCLK",
    [SYSCTL_CLK_SD1_HCLK] = "SYSCTL_CLK_SD1_HCLK",
    [SYSCTL_CLK_USB0_HCLK] = "SYSCTL_CLK_USB0_HCLK",
    [SYSCTL_CLK_USB1_HCLK] = "SYSCTL_CLK_USB1_HCLK",
    [SYSCTL_CLK_SSI0_HCLK] = "SYSCTL_CLK_SSI0_HCLK",
    [SYSCTL_CLK_SSI1_HCLK] = "SYSCTL_CLK_SSI1_HCLK",
    [SYSCTL_CLK_SSI2_HCLK] = "SYSCTL_CLK_SSI2_HCLK",
    [SYSCTL_CLK_SSI0_ACLK] = "SYSCTL_CLK_SSI0_ACLK",
    [SYSCTL_CLK_SSI1_CLK] = "SYSCTL_CLK_SSI1_CLK",
    [SYSCTL_CLK_SSI2_CLK] = "SYSCTL_CLK_SSI2_CLK",
    [SYSCTL_CLK_QSPI_ACLK] = "SYSCTL_CLK_QSPI_ACLK",
    [SYSCTL_CLK_SSI1_ACLK] = "SYSCTL_CLK_SSI1_ACLK",
    [SYSCTL_CLK_SSI2_ACLK] = "SYSCTL_CLK_SSI2_ACLK",
    [SYSCTL_CLK_SSI0_CLK] = "SYSCTL_CLK_SSI0_CLK",
    [SYSCTL_CLK_NOC_HS_SD_ACLK] = "SYSCTL_CLK_NOC_HS_SD_ACLK",
    [SYSCTL_CLK_SD0_ACLK] = "SYSCTL_CLK_SD0_ACLK",
    [SYSCTL_CLK_SD1_ACLK] = "SYSCTL_CLK_SD1_ACLK",
    [SYSCTL_CLK_SD_BCLK] = "SYSCTL_CLK_SD_BCLK",
    [SYSCTL_CLK_SD0_BCLK] = "SYSCTL_CLK_SD0_BCLK",
    [SYSCTL_CLK_SD1_BCLK] = "SYSCTL_CLK_SD1_BCLK",
    [SYSCTL_CLK_SD_CCLK] = "SYSCTL_CLK_SD_CCLK",
    [SYSCTL_CLK_PLL0_OCLK_DIV16] = "SYSCTL_CLK_PLL0_OCLK_DIV16",
    [SYSCTL_CLK_USB_REFCLK_50M] = "SYSCTL_CLK_USB_REFCLK_50M",
    [SYSCTL_CLK_USB0_REFCLK] = "SYSCTL_CLK_USB0_REFCLK",
    [SYSCTL_CLK_USB1_REFCLK] = "SYSCTL_CLK_USB1_REFCLK",
    [SYSCTL_CLK_SD_TMCLK] = "SYSCTL_CLK_SD_TMCLK",
    [SYSCTL_CLK_SD0_TMCLK] = "SYSCTL_CLK_SD0_TMCLK",
    [SYSCTL_CLK_SD1_TMCLK] = "SYSCTL_CLK_SD1_TMCLK",
    [SYSCTL_CLK_LS_PCLK] = "SYSCTL_CLK_LS_PCLK",
    [SYSCTL_CLK_UART0_PCLK] = "SYSCTL_CLK_UART0_PCLK",
    [SYSCTL_CLK_UART1_PCLK] = "SYSCTL_CLK_UART1_PCLK",
    [SYSCTL_CLK_UART2_PCLK] = "SYSCTL_CLK_UART2_PCLK",
    [SYSCTL_CLK_UART3_PCLK] = "SYSCTL_CLK_UART3_PCLK",
    [SYSCTL_CLK_UART4_PCLK] = "SYSCTL_CLK_UART4_PCLK",
    [SYSCTL_CLK_I2C0_PCLK] = "SYSCTL_CLK_I2C0_PCLK",
    [SYSCTL_CLK_I2C1_PCLK] = "SYSCTL_CLK_I2C1_PCLK",
    [SYSCTL_CLK_I2C2_PCLK] = "SYSCTL_CLK_I2C2_PCLK",
    [SYSCTL_CLK_I2C3_PCLK] = "SYSCTL_CLK_I2C3_PCLK",
    [SYSCTL_CLK_I2C4_PCLK] = "SYSCTL_CLK_I2C4_PCLK",
    [SYSCTL_CLK_GPIO_PCLK] = "SYSCTL_CLK_GPIO_PCLK",
    [SYSCTL_CLK_PWM_PCLK] = "SYSCTL_CLK_PWM_PCLK",
    [SYSCTL_CLK_JAMLINK0_PCLK] = "SYSCTL_CLK_JAMLINK0_PCLK",
    [SYSCTL_CLK_JAMLINK1_PCLK] = "SYSCTL_CLK_JAMLINK1_PCLK",
    [SYSCTL_CLK_JAMLINK2_PCLK] = "SYSCTL_CLK_JAMLINK2_PCLK",
    [SYSCTL_CLK_JAMLINK3_PCLK] = "SYSCTL_CLK_JAMLINK3_PCLK",
    [SYSCTL_CLK_ADC_PCLK] = "SYSCTL_CLK_ADC_PCLK",
    [SYSCTL_CLK_AUDIO_PCLK] = "SYSCTL_CLK_AUDIO_PCLK",
    [SYSCTL_CLK_CODEC_PCLK] = "SYSCTL_CLK_CODEC_PCLK",
    [SYSCTL_CLK_UART0_CLK] = "SYSCTL_CLK_UART0_CLK",
    [SYSCTL_CLK_UART1_CLK] = "SYSCTL_CLK_UART1_CLK",
    [SYSCTL_CLK_UART2_CLK] = "SYSCTL_CLK_UART2_CLK",
    [SYSCTL_CLK_UART3_CLK] = "SYSCTL_CLK_UART3_CLK",
    [SYSCTL_CLK_UART4_CLK] = "SYSCTL_CLK_UART4_CLK",
    [SYSCTL_CLK_JAMLINK0_CO] = "SYSCTL_CLK_JAMLINK0_CO",
    [SYSCTL_CLK_JAMLINK1_CO] = "SYSCTL_CLK_JAMLINK1_CO",
    [SYSCTL_CLK_JAMLINK2_CO] = "SYSCTL_CLK_JAMLINK2_CO",
    [SYSCTL_CLK_JAMLINK3_CO] = "SYSCTL_CLK_JAMLINK3_CO",
    [SYSCTL_CLK_I2C0_CLK] = "SYSCTL_CLK_I2C0_CLK",
    [SYSCTL_CLK_I2C1_CLK] = "SYSCTL_CLK_I2C1_CLK",
    [SYSCTL_CLK_I2C2_CLK] = "SYSCTL_CLK_I2C2_CLK",
    [SYSCTL_CLK_I2C3_CLK] = "SYSCTL_CLK_I2C3_CLK",
    [SYSCTL_CLK_I2C4_CLK] = "SYSCTL_CLK_I2C4_CLK",
    [SYSCTL_CLK_ADC_CLK] = "SYSCTL_CLK_ADC_CLK",
    [SYSCTL_CLK_GPIO_DBCLK] = "SYSCTL_CLK_GPIO_DBCLK",
    [SYSCTL_CLK_CODEC_ADC_MCLK] = "SYSCTL_CLK_CODEC_ADC_MCLK",
    [SYSCTL_CLK_CODEC_DAC_MCLK] = "SYSCTL_CLK_CODEC_DAC_MCLK",
    [SYSCTL_CLK_AUD_DEV_CLK] = "SYSCTL_CLK_AUD_DEV_CLK",
    [SYSCTL_CLK_PDM_CLK_ROOT] = "SYSCTL_CLK_PDM_CLK_ROOT",
    [SYSCTL_CLK_SYSCTL_PCLK] = "SYSCTL_CLK_SYSCTL_PCLK",
    [SYSCTL_CLK_WDT0_PCLK] = "SYSCTL_CLK_WDT0_PCLK",
    [SYSCTL_CLK_WDT1_PCLK] = "SYSCTL_CLK_WDT1_PCLK",
    [SYSCTL_CLK_TIMER_PCLK] = "SYSCTL_CLK_TIMER_PCLK",
    [SYSCTL_CLK_IOMUX_PCLK] = "SYSCTL_CLK_IOMUX_PCLK",
    [SYSCTL_CLK_MAILBOX_PCLK] = "SYSCTL_CLK_MAILBOX_PCLK",
    [SYSCTL_CLK_HDI_CLK] = "SYSCTL_CLK_HDI_CLK",
    [SYSCTL_CLK_STC_CLK] = "SYSCTL_CLK_STC_CLK",
    [SYSCTL_CLK_TS_CLK] = "SYSCTL_CLK_TS_CLK",
    [SYSCTL_CLK_WDT0_CLK] = "SYSCTL_CLK_WDT0_CLK",
    [SYSCTL_CLK_WDT1_CLK] = "SYSCTL_CLK_WDT1_CLK",
    [SYSCTL_CLK_TIMERX_PULSE_IN] = "SYSCTL_CLK_TIMERX_PULSE_IN",
    [SYSCTL_CLK_TIMER0_CLK] = "SYSCTL_CLK_TIMER0_CLK",
    [SYSCTL_CLK_TIMER1_CLK] = "SYSCTL_CLK_TIMER1_CLK",
    [SYSCTL_CLK_TIMER2_CLK] = "SYSCTL_CLK_TIMER2_CLK",
    [SYSCTL_CLK_TIMER3_CLK] = "SYSCTL_CLK_TIMER3_CLK",
    [SYSCTL_CLK_TIMER4_CLK] = "SYSCTL_CLK_TIMER4_CLK",
    [SYSCTL_CLK_TIMER5_CLK] = "SYSCTL_CLK_TIMER5_CLK",
    [SYSCTL_CLK_SRAM_CLK] = "SYSCTL_CLK_SRAM_CLK",
    [SYSCTL_CLK_SHRM_DIV2] = "SYSCTL_CLK_SHRM_DIV2",
    [SYSCTL_CLK_SHRM_AXIM_CLK] = "SYSCTL_CLK_SHRM_AXIM_CLK",
    [SYSCTL_CLK_NOC_SHRM_MCLK] = "SYSCTL_CLK_NOC_SHRM_MCLK",
    [SYSCTL_CLK_SHRM_AXIS_CLK] = "SYSCTL_CLK_SHRM_AXIS_CLK",
    [SYSCTL_CLK_DECOMP_PCLK] = "SYSCTL_CLK_DECOMP_PCLK",
    [SYSCTL_CLK_DECOMP_CLK] = "SYSCTL_CLK_DECOMP_CLK",
    [SYSCTL_CLK_SHRM_PCLK] = "SYSCTL_CLK_SHRM_PCLK",
    [SYSCTL_CLK_GSDMA_PCLK] = "SYSCTL_CLK_GSDMA_PCLK",
    [SYSCTL_CLK_GSDMA_CLK] = "SYSCTL_CLK_GSDMA_CLK",
    [SYSCTL_CLK_NOAI_2D_PCLK] = "SYSCTL_CLK_NOAI_2D_PCLK",
    [SYSCTL_CLK_NOAI_2D_CLK] = "SYSCTL_CLK_NOAI_2D_CLK",
    [SYSCTL_CLK_PDMA_PCLK] = "SYSCTL_CLK_PDMA_PCLK",
    [SYSCTL_CLK_PDMA_CLK] = "SYSCTL_CLK_PDMA_CLK",
    [SYSCTL_CLK_DDRC_CORE_CLK] = "SYSCTL_CLK_DDRC_CORE_CLK",
    [SYSCTL_CLK_DDR_BYPASSPCLK] = "SYSCTL_CLK_DDR_BYPASSPCLK",
    [SYSCTL_CLK_DDRC_PCLK] = "SYSCTL_CLK_DDRC_PCLK",
    [SYSCTL_CLK_NOC_DDRC_P0_CLK] = "SYSCTL_CLK_NOC_DDRC_P0_CLK",
    [SYSCTL_CLK_NOC_DDRC_P4_CLK] = "SYSCTL_CLK_NOC_DDRC_P4_CLK",
    [SYSCTL_CLK_NOC_DDRC_P3_ACLK] = "SYSCTL_CLK_NOC_DDRC_P3_ACLK",
    [SYSCTL_CLK_NOC_DDRC_P2_ACLK] = "SYSCTL_CLK_NOC_DDRC_P2_ACLK",
    [SYSCTL_CLK_NOC_DDRC_P1_ACLK] = "SYSCTL_CLK_NOC_DDRC_P1_ACLK",
    [SYSCTL_CLK_ISP_CFG_CLK] = "SYSCTL_CLK_ISP_CFG_CLK",
    [SYSCTL_CLK_ISP_PCLK] = "SYSCTL_CLK_ISP_PCLK",
    [SYSCTL_CLK_CSI0_CFG_CLK] = "SYSCTL_CLK_CSI0_CFG_CLK",
    [SYSCTL_CLK_CSI1_CFG_CLK] = "SYSCTL_CLK_CSI1_CFG_CLK",
    [SYSCTL_CLK_CSI2_CFG_CLK] = "SYSCTL_CLK_CSI2_CFG_CLK",
    [SYSCTL_CLK_ISP_DPHY_PCLK] = "SYSCTL_CLK_ISP_DPHY_PCLK",
    [SYSCTL_CLK_CSI0_PIXEL_CLK] = "SYSCTL_CLK_CSI0_PIXEL_CLK",
    [SYSCTL_CLK_CSI1_PIXEL_CLK] = "SYSCTL_CLK_CSI1_PIXEL_CLK",
    [SYSCTL_CLK_CSI2_PIXEL_CLK] = "SYSCTL_CLK_CSI2_PIXEL_CLK",
    [SYSCTL_CLK_CSI0_PCLK] = "SYSCTL_CLK_CSI0_PCLK",
    [SYSCTL_CLK_CSI1_PCLK] = "SYSCTL_CLK_CSI1_PCLK",
    [SYSCTL_CLK_CSI2_PCLK] = "SYSCTL_CLK_CSI2_PCLK",
    [SYSCTL_CLK_ISP_CLK] = "SYSCTL_CLK_ISP_CLK",
    [SYSCTL_CLK_ISP_MCM_CLK] = "SYSCTL_CLK_ISP_MCM_CLK",
    [SYSCTL_CLK_ISP_HCLK] = "SYSCTL_CLK_ISP_HCLK",
    [SYSCTL_CLK_ISP_ACLK] = "SYSCTL_CLK_ISP_ACLK",
    [SYSCTL_CLK_ISPDWE_CLK] = "SYSCTL_CLK_ISPDWE_CLK",
    [SYSCTL_CLK_ISPVSE_CLK] = "SYSCTL_CLK_ISPVSE_CLK",
    [SYSCTL_CLK_ISP_DW_HCLK] = "SYSCTL_CLK_ISP_DW_HCLK",
    [SYSCTL_CLK_ISP_DW_ACLK] = "SYSCTL_CLK_ISP_DW_ACLK",
    [SYSCTL_CLK_MCLK0] = "SYSCTL_CLK_MCLK0",
    [SYSCTL_CLK_MCLK1] = "SYSCTL_CLK_MCLK1",
    [SYSCTL_CLK_MCLK2] = "SYSCTL_CLK_MCLK2",
    [SYSCTL_CLK_DPU_CLK] = "SYSCTL_CLK_DPU_CLK",
    [SYSCTL_CLK_DPU_ACLK] = "SYSCTL_CLK_DPU_ACLK",
    [SYSCTL_CLK_DPU_PCLK] = "SYSCTL_CLK_DPU_PCLK",
    [SYSCTL_CLK_DISP_HCLK] = "SYSCTL_CLK_DISP_HCLK",
    [SYSCTL_CLK_DISP_ACLK] = "SYSCTL_CLK_DISP_ACLK",
    [SYSCTL_CLK_DISP_CLKEXT] = "SYSCTL_CLK_DISP_CLKEXT",
    [SYSCTL_CLK_CLK_GPU] = "SYSCTL_CLK_CLK_GPU",
    [SYSCTL_CLK_DPIPCLK] = "SYSCTL_CLK_DPIPCLK",
    [SYSCTL_CLK_DISP_CFGCLK] = "SYSCTL_CLK_DISP_CFGCLK",
    [SYSCTL_CLK_DISP_REFCLK] = "SYSCTL_CLK_DISP_REFCLK",
    [SYSCTL_CLK_DISP_VO_PCLK] = "SYSCTL_CLK_DISP_VO_PCLK",
    [SYSCTL_CLK_DISP_DSI_PCLK] = "SYSCTL_CLK_DISP_DSI_PCLK",
    [SYSCTL_CLK_DISP_DSI_PHY_PCLK] = "SYSCTL_CLK_DISP_DSI_PHY_PCLK",
    [SYSCTL_CLK_DISP_GPU_HCLK] = "SYSCTL_CLK_DISP_GPU_HCLK",
    [SYSCTL_CLK_DISP_VO_ACLK] = "SYSCTL_CLK_DISP_VO_ACLK",
    [SYSCTL_CLK_DISP_GPU_ACLK] = "SYSCTL_CLK_DISP_GPU_ACLK",
    [SYSCTL_CLK_SEC_PCLK] = "SYSCTL_CLK_SEC_PCLK",
    [SYSCTL_CLK_FIXCLK] = "SYSCTL_CLK_FIXCLK",
    [SYSCTL_CLK_SEC_ACLK] = "SYSCTL_CLK_SEC_ACLK",
    [SYSCTL_CLK_USB_TEST_CLK_GATE] = "SYSCTL_CLK_USB_TEST_CLK_GATE",
    [SYSCTL_CLK_USB_CLK480] = "SYSCTL_CLK_USB_CLK480",
    [SYSCTL_CLK_USB_CLK100] = "SYSCTL_CLK_USB_CLK100",
    [SYSCTL_CLK_DPHY_TEST_CLK] = "SYSCTL_CLK_DPHY_TEST_CLK",
    [SYSCTL_CLK_SPI2AXI_ACLK] = "SYSCTL_CLK_SPI2AXI_ACLK",
    [SYSCTL_CLK_CPU1_CORE_CLK_ROOT] = "SYSCTL_CLK_CPU1_CORE_CLK_ROOT",
    [SYSCTL_CLK_CPU1_CORE_CLK] = "SYSCTL_CLK_CPU1_CORE_CLK",
    [SYSCTL_CLK_NOC_CPU1_CLK] = "SYSCTL_CLK_NOC_CPU1_CLK",
    [SYSCTL_CLK_DDRC_P0_CLK] = "SYSCTL_CLK_DDRC_P0_CLK",
    [SYSCTL_CLK_CPU0_CORE_CLK] = "SYSCTL_CLK_CPU0_CORE_CLK",
    [SYSCTL_CLK_NOC_CPU0_CLK] = "SYSCTL_CLK_NOC_CPU0_CLK",
    [SYSCTL_CLK_DDRC_P4_CLK] = "SYSCTL_CLK_DDRC_P4_CLK",
    [SYSCTL_CLK_NOC_AI_ACLK] = "SYSCTL_CLK_NOC_AI_ACLK",
    [SYSCTL_CLK_DDRC_P3_CLK] = "SYSCTL_CLK_DDRC_P3_CLK",
    [SYSCTL_CLK_VPU_CLK_ROOT] = "SYSCTL_CLK_VPU_CLK_ROOT",
    [SYSCTL_CLK_NOC_VPU_ACLK] = "SYSCTL_CLK_NOC_VPU_ACLK",
    [SYSCTL_CLK_DDRC_P2_CLK] = "SYSCTL_CLK_DDRC_P2_CLK",
    [SYSCTL_CLK_NOC_VPU_CFG_CLK] = "SYSCTL_CLK_NOC_VPU_CFG_CLK",
    [SYSCTL_CLK_NOC_PMU_PCLK] = "SYSCTL_CLK_NOC_PMU_PCLK",
    [SYSCTL_CLK_NOC_HS_SUB_HCLK] = "SYSCTL_CLK_NOC_HS_SUB_HCLK",
    [SYSCTL_CLK_SD_ACLK] = "SYSCTL_CLK_SD_ACLK",
    [SYSCTL_CLK_NOC_HS_QSPI_ACLK] = "SYSCTL_CLK_NOC_HS_QSPI_ACLK",
    [SYSCTL_CLK_SD0_CCLK_TX] = "SYSCTL_CLK_SD0_CCLK_TX",
    [SYSCTL_CLK_SD1_CCLK_TX] = "SYSCTL_CLK_SD1_CCLK_TX",
    [SYSCTL_CLK_NOC_LS_PCLK] = "SYSCTL_CLK_NOC_LS_PCLK",
    [SYSCTL_CLK_NOC_SHRM_PCLK] = "SYSCTL_CLK_NOC_SHRM_PCLK",
    [SYSCTL_CLK_NOC_DDRC_PCLK] = "SYSCTL_CLK_NOC_DDRC_PCLK",
    [SYSCTL_CLK_DDRC_P1_CLK] = "SYSCTL_CLK_DDRC_P1_CLK",
    [SYSCTL_CLK_NOC_ISP_HCLK] = "SYSCTL_CLK_NOC_ISP_HCLK",
    [SYSCTL_CLK_NOC_ISP_ACLK] = "SYSCTL_CLK_NOC_ISP_ACLK",
    [SYSCTL_CLK_NOC_DPU_ACLK] = "SYSCTL_CLK_NOC_DPU_ACLK",
    [SYSCTL_CLK_NOC_DPU_PCLK] = "SYSCTL_CLK_NOC_DPU_PCLK",
    [SYSCTL_CLK_NOC_DISP_HCLK] = "SYSCTL_CLK_NOC_DISP_HCLK",
    [SYSCTL_CLK_NOC_DISP_ACLK] = "SYSCTL_CLK_NOC_DISP_ACLK",
    [SYSCTL_CLK_NOC_SPI2AXI_ACLK] = "SYSCTL_CLK_NOC_SPI2AXI_ACLK",
    [SYSCTL_CLK_NOC_HS_OSPI_ACLK] = "SYSCTL_CLK_NOC_HS_OSPI_ACLK",
    [SYSCTL_CLK_NOC_SEC_ACLK] = "SYSCTL_CLK_NOC_SEC_ACLK",
    [SYSCTL_CLK_NOC_SEC_PCLK] = "SYSCTL_CLK_NOC_SEC_PCLK",
};

static const char *media_clk_name(sysctl_media_clk_node_e leaf)
{
    if (leaf >= 0 && leaf < SYSCTL_CLK_NODE_MAX && media_clk_names[leaf] != RT_NULL)
        return media_clk_names[leaf];

    return "-";
}

static int media_clk_refcount(sysctl_media_clk_node_e leaf)
{
    int idx = media_leaf_index(leaf);
    uint32_t refcount;
    rt_base_t level;

    if (idx < 0)
        return -1;

    level = rt_hw_interrupt_disable();
    refcount = media_leaf_ref_count[idx];
    rt_hw_interrupt_enable(level);

    return (int)refcount;
}

static long k230_media_clk_dump(void)
{
    rt_kprintf("%-36s %-36s %-4s %-4s %-10s\n", "media_clock", "parent", "hw", "ref", "rate_hz");
    for (unsigned int i = 0; i < sizeof(media_clk_nodes) / sizeof(media_clk_nodes[0]); i++) {
        sysctl_media_clk_node_e leaf = media_clk_nodes[i];
        sysctl_media_clk_node_e parent = sysctl_media_clk_get_leaf_parent(leaf);
        int refcount = media_clk_refcount(leaf);
        char refbuf[12];

        if (media_clk_names[leaf] == RT_NULL)
            continue;

        if (refcount >= 0)
            rt_snprintf(refbuf, sizeof(refbuf), "%d", refcount);
        else
            rt_snprintf(refbuf, sizeof(refbuf), "-");

        rt_kprintf("%-36s %-36s %-4s %-4s %-10u\n",
                   media_clk_name(leaf),
                   media_clk_name(parent),
                   sysctl_media_clk_get_leaf_en(leaf) ? "on" : "off",
                   refbuf,
                   sysctl_media_clk_get_leaf_freq(leaf));
    }

    return 0;
}
MSH_CMD_EXPORT(k230_media_clk_dump, dump K230 media clock resource table);

static void sysctl_media_clk_locks_init(void)
{
    for (unsigned int i = 0; i < sizeof(media_clk_locks) / sizeof(media_clk_locks[0]); i++)
        rt_mutex_init(&media_clk_locks[i], "mclk", RT_IPC_FLAG_PRIO);
    media_clk_locks_ready = true;
}

int rt_hw_sysctl_clk_init(void)
{
    sysctl_clk = rt_ioremap((void*)CMU_BASE_ADDR, CMU_IO_SIZE);
    if(!sysctl_clk) {
        rt_kprintf("sysctl_clk ioremap error\n");
        return -1;
    }

    sysctl_media_clk_locks_init();

    return 0;
}
INIT_BOARD_EXPORT(rt_hw_sysctl_clk_init);
