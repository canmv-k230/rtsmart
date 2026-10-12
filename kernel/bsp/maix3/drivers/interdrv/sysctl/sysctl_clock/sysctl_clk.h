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

#ifndef __SYSCTL_CLK_H__
#define __SYSCTL_CLK_H__

/* created by yangfan */

#include <stdint.h>
#include <stdbool.h>
// #include "platform.h"

#define SYSCTL_BASE_ADDR         0x91100000
#define SYSCTL_CLK_BASE_ADDR    (SYSCTL_BASE_ADDR + 0x0000)     /* SYSCTL_BASE_ADDR=0x9110_0000 */

typedef struct sysctl_clk {
    volatile uint32_t cpu0_clk_cfg;             /* 0x00 */
    volatile uint32_t cpu1_clk_cfg;             /* 0x04 */
    volatile uint32_t ai_clk_cfg;               /* 0x08 */
    volatile uint32_t vpu_clk_cfg;              /* 0x0c */
    volatile uint32_t pmu_clk_cfg;              /* 0x10 */
    volatile uint32_t reserved0[1];             /* 0x14 */
    volatile uint32_t hs_clken_cfg;             /* 0x18 */
    volatile uint32_t hs_sdclk_cfg;             /* 0x1c */
    volatile uint32_t hs_spi_cfg;               /* 0x20 */
    volatile uint32_t ls_clken_cfg0;            /* 0x24 */
    volatile uint32_t ls_clken_cfg1;            /* 0x28 */
    volatile uint32_t uart_i2c_clkdiv_cfg;      /* 0x2c */
    volatile uint32_t ls_clkdiv_cfg;            /* 0x30 */
    volatile uint32_t audio_clkdiv_cfg;         /* 0x34 */
    volatile uint32_t codec_adc_mclkdiv_cfg;    /* 0x38 */
    volatile uint32_t codec_dac_mclkdiv_cfg;    /* 0x3c */
    volatile uint32_t pdm_clkdiv_cfg0;          /* 0x40 */
    volatile uint32_t pdm_clkdiv_cfg1;          /* 0x44 */
    volatile uint32_t reserved1[2];             /* 0x48 0x4c */
    volatile uint32_t sysctl_clken_cfg;         /* 0x50 */
    volatile uint32_t timer_clk_cfg;            /* 0x54 */
    volatile uint32_t sysctl_clk_div_cfg;       /* 0x58 */
    volatile uint32_t shrm_clk_cfg;             /* 0x5c */
    volatile uint32_t ddr_clk_cfg;              /* 0x60 */
    volatile uint32_t isp_clken_cfg;            /* 0x64 */
    volatile uint32_t isp_clkdiv_cfg;           /* 0x68 */
    volatile uint32_t mclk_cfg;                 /* 0x6c */
    volatile uint32_t dpu_clk_cfg;              /* 0x70 */
    volatile uint32_t vo_clk_cfg;               /* 0x74 */
    volatile uint32_t disp_clk_div;             /* 0x78 */
    volatile uint32_t reserved2[1];             /* 0x7c */
    volatile uint32_t sec_clk_div;              /* 0x80 */
    volatile uint32_t reserved3[31];             /* 0x84 0x88 0x8c 0x90 0x94 0x98 0x9c 0xa0-0xac 0xb0-0xbc 0xc0-0xcc 0xd0-0xdc 0xe0-0xec 0xf0-0xfc*/
    volatile uint32_t usb_test_clk_div;         /* 0x100 */
    volatile uint32_t dphy_test_clk_div;        /* 0x104 */
    volatile uint32_t spi2axi_clk_div;          /* 0x108 */
} sysctl_clk_t;

typedef enum {
    /*
     * Clock tree reference: MAIX3 clock/reset V1.0 and register map V1.0.
     * This enum remains in CMU register order. A -> B means B has one fixed
     * parent. MUX[sel: parent, ...] -> B lists every legal parent; reset is
     * the parent selected by the register reset value.
     *
     * OSC24M -> PLL{0,1,2,3}_OCLK -> PLLx_OCLK_DIV{2,3,4}
     * ROOT_TIMERX_PULSE_IN is the external timer-pulse root.
     */
    /* Root clocks (sysctl_boot) */
    SYSCTL_CLK_OSC24M = 0,
    SYSCTL_CLK_ROOT_TIMERX_PULSE_IN,
    SYSCTL_CLK_PLL0_OCLK,
    SYSCTL_CLK_PLL0_OCLK_DIV2,
    SYSCTL_CLK_PLL0_OCLK_DIV3,
    SYSCTL_CLK_PLL0_OCLK_DIV4,
    SYSCTL_CLK_PLL1_OCLK,
    SYSCTL_CLK_PLL1_OCLK_DIV2,
    SYSCTL_CLK_PLL1_OCLK_DIV3,
    SYSCTL_CLK_PLL1_OCLK_DIV4,
    SYSCTL_CLK_PLL2_OCLK,
    SYSCTL_CLK_PLL2_OCLK_DIV2,
    SYSCTL_CLK_PLL2_OCLK_DIV3,
    SYSCTL_CLK_PLL2_OCLK_DIV4,
    SYSCTL_CLK_PLL3_OCLK,
    SYSCTL_CLK_PLL3_OCLK_DIV2,
    SYSCTL_CLK_PLL3_OCLK_DIV3,
    SYSCTL_CLK_PLL3_OCLK_DIV4,
    SYSCTL_CLK_ROOT_MAX,
    /*
     * PLL0_OCLK_DIV2 -> CPU0_CORE_CLK_ROOT
     *     -> {CPU0_CORE_CLK, CPU0_PLICLK, CPU0_ACLK,
     *         NOC_DDRC_P4_CLK, NOC_CPU0_CLK}
     * PLL0_OCLK_DIV4 -> {TDI_PCLK, CPU0_PCLK}
     */
    /* CMU 0x00: CPU0_CLK_CFG */
    SYSCTL_CLK_CPU0_CORE_CLK_ROOT,
    SYSCTL_CLK_CPU0_PLICLK,
    SYSCTL_CLK_CPU0_ACLK,
    SYSCTL_CLK_TDI_PCLK,
    SYSCTL_CLK_CPU0_PCLK,
    /*
     * MUX[0: PLL1_OCLK_DIV2, 1: PLL3_OCLK, 2: PLL0_OCLK]
     *     -> CPU1_CORE_CLK_SRC -> CPU1_CORE_CLK_ROOT
     *     -> {CPU1_CORE_CLK, CPU1_PLICLK, CPU1_ACLK,
     *         NOC_DDRC_P0_CLK, NOC_CPU1_CLK}
     * PLL0_OCLK_DIV4 -> CPU1_PCLK
     */
    /* CMU 0x04: CPU1_CLK_CFG */
    SYSCTL_CLK_CPU1_CORE_CLK_SRC,
    SYSCTL_CLK_CPU1_PLICLK,
    SYSCTL_CLK_CPU1_ACLK,
    SYSCTL_CLK_CPU1_PCLK,
    /*
     * MUX[0: PLL1_OCLK_DIV2, 1: PLL3_OCLK_DIV2] -> AI_CORE_CLK
     * PLL0_OCLK_DIV4 -> NOC_AI_ACLK -> AI_ACLK
     */
    /* CMU 0x08: AI_CLK_CFG */
    SYSCTL_CLK_AI_CORE_CLK,
    SYSCTL_CLK_AI_ACLK,
    /*
     * PLL0_OCLK_DIV2 -> VPU_CLK_ROOT -> {VPU_CLK, VPU_ACLK_ROOT}
     * VPU_ACLK_ROOT -> {NOC_VPU_ACLK -> VPU_ACLK, NOC_DDRC_P2_ACLK}
     * PLL0_OCLK_DIV4 -> NOC_VPU_CFG_CLK -> VPU_CFG_CLK
     */
    /* CMU 0x0c: VPU_CLK_CFG */
    SYSCTL_CLK_VPU_CLK,
    SYSCTL_CLK_VPU_ACLK_ROOT,
    SYSCTL_CLK_VPU_ACLK,
    SYSCTL_CLK_VPU_CFG_CLK,
    /* OSC24M -> NOC_PMU_PCLK -> PMU_PCLK */
    /* CMU 0x10: PMU_CLK_CFG */
    SYSCTL_CLK_PMU_PCLK,
    /*
     * PLL0_OCLK_DIV4 -> NOC_HS_OSPI_HCLK -> HS_HCLK_HIGH -> SSI0_HCLK
     * NOC_HS_OSPI_HCLK -> NOC_HS_SUB_HCLK -> HS_HCLK -> bus HCLK gates
     * PLL0_OCLK_DIV4 -> NOC_HS_OSPI_ACLK -> SSI0_ACLK
     * PLL0_OCLK_DIV4 -> NOC_HS_QSPI_ACLK -> QSPI_ACLK
     *     -> {SSI1_ACLK, SSI2_ACLK}
     * PLL2_OCLK_DIV4 -> NOC_HS_SD_ACLK -> SD_ACLK
     *     -> {SD0_ACLK, SD1_ACLK, SD0_BCLK, SD1_BCLK}
     * PLL2_OCLK_DIV4 -> SD_BCLK (shared BCLK gate/div control)
     * PLL0_OCLK -> PLL0_OCLK_DIV16 -> USB_REFCLK_50M
     * MUX[OSC24M, USB_REFCLK_50M] -> {USB0_REFCLK, USB1_REFCLK}
     * OSC24M -> SD_TMCLK -> {SD0_TMCLK, SD1_TMCLK}
     */
    /* CMU 0x18-0x20: high-speed clocks */
    SYSCTL_CLK_NOC_HS_OSPI_HCLK,
    SYSCTL_CLK_HS_HCLK_HIGH,
    SYSCTL_CLK_HS_HCLK,
    SYSCTL_CLK_SD0_HCLK,
    SYSCTL_CLK_SD1_HCLK,
    SYSCTL_CLK_USB0_HCLK,
    SYSCTL_CLK_USB1_HCLK,
    SYSCTL_CLK_SSI0_HCLK,
    SYSCTL_CLK_SSI1_HCLK,
    SYSCTL_CLK_SSI2_HCLK,
    SYSCTL_CLK_SSI0_ACLK,
    SYSCTL_CLK_SSI1_CLK,
    SYSCTL_CLK_SSI2_CLK,
    SYSCTL_CLK_QSPI_ACLK,
    SYSCTL_CLK_SSI1_ACLK,
    SYSCTL_CLK_SSI2_ACLK,
    SYSCTL_CLK_SSI0_CLK,
    SYSCTL_CLK_NOC_HS_SD_ACLK,
    SYSCTL_CLK_SD0_ACLK,
    SYSCTL_CLK_SD1_ACLK,
    SYSCTL_CLK_SD_BCLK,
    SYSCTL_CLK_SD0_BCLK,
    SYSCTL_CLK_SD1_BCLK,
    SYSCTL_CLK_SD_CCLK,
    SYSCTL_CLK_SD0_CCLK_TX,
    SYSCTL_CLK_SD1_CCLK_TX,
    SYSCTL_CLK_PLL0_OCLK_DIV16,
    SYSCTL_CLK_USB_REFCLK_50M,
    SYSCTL_CLK_USB0_REFCLK,
    SYSCTL_CLK_USB1_REFCLK,
    SYSCTL_CLK_SD_TMCLK,
    SYSCTL_CLK_SD0_TMCLK,
    SYSCTL_CLK_SD1_TMCLK,
    /*
     * PLL0_OCLK_DIV4 -> NOC_LS_PCLK -> LS_PCLK -> peripheral PCLK gates
     * PLL0_OCLK_DIV16 -> {UART0..4_CLK, JAMLINK0..3_CO}
     * PLL0_OCLK_DIV4 -> {I2C0..4_CLK, ADC_CLK}
     * OSC24M -> GPIO_DBCLK
     */
    /* CMU 0x24-0x30: low-speed clocks */
    SYSCTL_CLK_LS_PCLK,
    SYSCTL_CLK_UART0_PCLK,
    SYSCTL_CLK_UART1_PCLK,
    SYSCTL_CLK_UART2_PCLK,
    SYSCTL_CLK_UART3_PCLK,
    SYSCTL_CLK_UART4_PCLK,
    SYSCTL_CLK_I2C0_PCLK,
    SYSCTL_CLK_I2C1_PCLK,
    SYSCTL_CLK_I2C2_PCLK,
    SYSCTL_CLK_I2C3_PCLK,
    SYSCTL_CLK_I2C4_PCLK,
    SYSCTL_CLK_GPIO_PCLK,
    SYSCTL_CLK_PWM_PCLK,
    SYSCTL_CLK_JAMLINK0_PCLK,
    SYSCTL_CLK_JAMLINK1_PCLK,
    SYSCTL_CLK_JAMLINK2_PCLK,
    SYSCTL_CLK_JAMLINK3_PCLK,
    SYSCTL_CLK_ADC_PCLK,
    SYSCTL_CLK_AUDIO_PCLK,
    SYSCTL_CLK_CODEC_PCLK,
    SYSCTL_CLK_UART0_CLK,
    SYSCTL_CLK_UART1_CLK,
    SYSCTL_CLK_UART2_CLK,
    SYSCTL_CLK_UART3_CLK,
    SYSCTL_CLK_UART4_CLK,
    SYSCTL_CLK_JAMLINK0_CO,
    SYSCTL_CLK_JAMLINK1_CO,
    SYSCTL_CLK_JAMLINK2_CO,
    SYSCTL_CLK_JAMLINK3_CO,
    SYSCTL_CLK_I2C0_CLK,
    SYSCTL_CLK_I2C1_CLK,
    SYSCTL_CLK_I2C2_CLK,
    SYSCTL_CLK_I2C3_CLK,
    SYSCTL_CLK_I2C4_CLK,
    SYSCTL_CLK_ADC_CLK,
    SYSCTL_CLK_GPIO_DBCLK,
    /*
     * PLL0_OCLK_DIV4 -> {CODEC_ADC_MCLK, CODEC_DAC_MCLK, AUD_DEV_CLK,
     *                    PDM_CLK_ROOT}
     */
    /* CMU 0x34-0x44: audio clocks */
    SYSCTL_CLK_CODEC_ADC_MCLK,
    SYSCTL_CLK_CODEC_DAC_MCLK,
    SYSCTL_CLK_AUD_DEV_CLK,
    SYSCTL_CLK_PDM_CLK_ROOT,
    /*
     * PLL0_OCLK_DIV16 -> SYSCTL_PCLK
     *     -> {WDT0_PCLK, WDT1_PCLK, TIMER_PCLK, IOMUX_PCLK, MAILBOX_PCLK}
     * PLL0_OCLK_DIV4 -> HDI_CLK; PLL1_OCLK_DIV4 -> STC_CLK
     * OSC24M -> {TS_CLK, WDT0_CLK, WDT1_CLK}
     */
    /* CMU 0x50-0x58: system-control clocks */
    SYSCTL_CLK_SYSCTL_PCLK,
    SYSCTL_CLK_WDT0_PCLK,
    SYSCTL_CLK_WDT1_PCLK,
    SYSCTL_CLK_TIMER_PCLK,
    SYSCTL_CLK_IOMUX_PCLK,
    SYSCTL_CLK_MAILBOX_PCLK,
    SYSCTL_CLK_HDI_CLK,
    SYSCTL_CLK_STC_CLK,
    SYSCTL_CLK_TS_CLK,
    SYSCTL_CLK_WDT0_CLK,
    SYSCTL_CLK_WDT1_CLK,
    /*
     * PLL0_OCLK_DIV16 -> TIMER0..5_CLK
     * ROOT_TIMERX_PULSE_IN -> TIMERX_PULSE_IN
     * MUX[0: PLL0_OCLK_DIV16, 1: TIMERX_PULSE_IN] -> TIMER_n_CLK
     *     (n = 0..5; reset: ROOT_TIMERX_PULSE_IN, select 1)
     */
    /* CMU 0x54: timer clocks */
    SYSCTL_CLK_TIMERX_PULSE_IN,
    SYSCTL_CLK_TIMER0_CLK,
    SYSCTL_CLK_TIMER1_CLK,
    SYSCTL_CLK_TIMER2_CLK,
    SYSCTL_CLK_TIMER3_CLK,
    SYSCTL_CLK_TIMER4_CLK,
    SYSCTL_CLK_TIMER5_CLK,
    /*
     * MUX[PLL0_OCLK_DIV2, PLL3_OCLK_DIV2] -> SRAM_CLK
     * SRAM_CLK -> {SHRM_DIV2 -> SHRM_AXIS_CLK, DECOMP_CLK}
     * PLL0_OCLK_DIV4 -> NOC_SHRM_MCLK -> SHRM_AXIM_CLK
     *     -> {GSDMA_CLK, NOAI_2D_CLK, PDMA_CLK}
     * PLL0_OCLK_DIV4 -> NOC_SHRM_PCLK -> SHRM_PCLK
     */
    /* CMU 0x5c: SHRM clocks */
    SYSCTL_CLK_SRAM_CLK,
    SYSCTL_CLK_SHRM_DIV2,
    SYSCTL_CLK_SHRM_AXIM_CLK,
    SYSCTL_CLK_NOC_SHRM_MCLK,
    SYSCTL_CLK_SHRM_AXIS_CLK,
    SYSCTL_CLK_DECOMP_PCLK,
    SYSCTL_CLK_DECOMP_CLK,
    SYSCTL_CLK_SHRM_PCLK,
    SYSCTL_CLK_GSDMA_PCLK,
    SYSCTL_CLK_GSDMA_CLK,
    SYSCTL_CLK_NOAI_2D_PCLK,
    SYSCTL_CLK_NOAI_2D_CLK,
    SYSCTL_CLK_PDMA_PCLK,
    SYSCTL_CLK_PDMA_CLK,
    /*
     * MUX[PLL0_OCLK_DIV2, PLL0_OCLK_DIV3, PLL2_OCLK_DIV4] -> DDRC_CORE_CLK
     * PLL2_OCLK_DIV4 -> DDR_BYPASSPCLK
     * PLL0_OCLK_DIV4 -> NOC_DDRC_PCLK -> DDRC_PCLK
     * CPU1_CORE_CLK_ROOT -> NOC_DDRC_P0_CLK
     * CPU0_CORE_CLK_ROOT -> NOC_DDRC_P4_CLK
     * VPU_ACLK_ROOT -> NOC_DDRC_P2_ACLK
     * PLL0_OCLK_DIV4 -> {NOC_DDRC_P3_ACLK, NOC_DDRC_P1_ACLK}
     */
    /* CMU 0x60: DDR clocks and ports */
    SYSCTL_CLK_DDRC_CORE_CLK,
    SYSCTL_CLK_DDR_BYPASSPCLK,
    SYSCTL_CLK_DDRC_PCLK,
    SYSCTL_CLK_NOC_DDRC_P0_CLK,
    SYSCTL_CLK_NOC_DDRC_P4_CLK,
    SYSCTL_CLK_NOC_DDRC_P3_ACLK,
    SYSCTL_CLK_NOC_DDRC_P2_ACLK,
    SYSCTL_CLK_NOC_DDRC_P1_ACLK,
    /*
     * PLL1_OCLK_DIV4 -> ISP_CFG_CLK -> CSI/DPHY configuration clocks
     * PLL2_OCLK_DIV4 -> CSI0..2_PIXEL_CLK
     * PLL0_OCLK_DIV4 -> NOC_ISP_HCLK -> ISP_HCLK -> peripheral PCLKs
     * PLL0_OCLK_DIV4 -> NOC_ISP_ACLK -> ISP_ACLK
     * MUX[PLL0_OCLK_DIV4, PLL2_OCLK_DIV4] -> ISP_CLK -> ISP_MCM_CLK
     */
    /* CMU 0x64-0x68: ISP clocks */
    SYSCTL_CLK_ISP_CFG_CLK,
    SYSCTL_CLK_ISP_PCLK,
    SYSCTL_CLK_CSI0_CFG_CLK,
    SYSCTL_CLK_CSI1_CFG_CLK,
    SYSCTL_CLK_CSI2_CFG_CLK,
    SYSCTL_CLK_ISP_DPHY_PCLK,
    SYSCTL_CLK_CSI0_PIXEL_CLK,
    SYSCTL_CLK_CSI1_PIXEL_CLK,
    SYSCTL_CLK_CSI2_PIXEL_CLK,
    SYSCTL_CLK_CSI0_PCLK,
    SYSCTL_CLK_CSI1_PCLK,
    SYSCTL_CLK_CSI2_PCLK,
    SYSCTL_CLK_ISP_CLK,
    SYSCTL_CLK_ISP_MCM_CLK,
    SYSCTL_CLK_ISP_HCLK,
    SYSCTL_CLK_ISP_ACLK,
    SYSCTL_CLK_ISPDWE_CLK,
    SYSCTL_CLK_ISPVSE_CLK,
    SYSCTL_CLK_ISP_DW_HCLK,
    SYSCTL_CLK_ISP_DW_ACLK,
    /*
     * MUX[PLL1_OCLK_DIV3, PLL1_OCLK_DIV4, PLL0_OCLK_DIV4]
     *     -> {MCLK0, MCLK1, MCLK2}
     */
    /* CMU 0x6c: MCLK clocks */
    SYSCTL_CLK_MCLK0,
    SYSCTL_CLK_MCLK1,
    SYSCTL_CLK_MCLK2,
    /*
     * PLL1_OCLK_DIV4 -> DPU_CLK
     * PLL0_OCLK_DIV4 -> {NOC_DPU_ACLK -> DPU_ACLK,
     *                    NOC_DPU_PCLK -> DPU_PCLK}
     */
    /* CMU 0x70: DPU clocks */
    SYSCTL_CLK_DPU_CLK,
    SYSCTL_CLK_DPU_ACLK,
    SYSCTL_CLK_DPU_PCLK,
    /*
     * PLL0_OCLK_DIV4 -> NOC_DISP_HCLK -> DISP_HCLK
     * PLL0_OCLK_DIV4 -> NOC_DISP_ACLK -> DISP_ACLK
     * PLL0_OCLK_DIV3 -> {DISP_CLKEXT, CLK_GPU}
     * PLL1_OCLK_DIV4 -> {DPIPCLK, DISP_CFGCLK, display PCLKs}
     * OSC24M -> DISP_REFCLK
     */
    /* CMU 0x74-0x78: display clocks */
    SYSCTL_CLK_DISP_HCLK,
    SYSCTL_CLK_DISP_ACLK,
    SYSCTL_CLK_DISP_CLKEXT,
    SYSCTL_CLK_CLK_GPU,
    SYSCTL_CLK_DPIPCLK,
    SYSCTL_CLK_DISP_CFGCLK,
    SYSCTL_CLK_DISP_REFCLK,
    SYSCTL_CLK_DISP_VO_PCLK,
    SYSCTL_CLK_DISP_DSI_PCLK,
    SYSCTL_CLK_DISP_DSI_PHY_PCLK,
    SYSCTL_CLK_DISP_GPU_HCLK,
    SYSCTL_CLK_DISP_VO_ACLK,
    SYSCTL_CLK_DISP_GPU_ACLK,
    /*
     * PLL0_OCLK_DIV4 -> NOC_SEC_PCLK -> SEC_PCLK
     * PLL1_OCLK_DIV4 -> {FIXCLK, NOC_SEC_ACLK -> SEC_ACLK}
     */
    /* CMU 0x80: security clocks */
    SYSCTL_CLK_SEC_PCLK,
    SYSCTL_CLK_FIXCLK,
    SYSCTL_CLK_SEC_ACLK,
    /*
     * USB_TEST_CLK_GATE is shared by USB_CLK480 and USB_CLK100.
     * PLL1_OCLK -> USB_CLK480; PLL0_OCLK_DIV4 -> USB_CLK100
     * PLL0_OCLK -> DPHY_TEST_CLK
     * PLL0_OCLK_DIV4 -> NOC_SPI2AXI_ACLK -> SPI2AXI_ACLK
     */
    /* CMU 0x100-0x108: test and bridge clocks */
    SYSCTL_CLK_USB_TEST_CLK_GATE,
    SYSCTL_CLK_USB_CLK480,
    SYSCTL_CLK_USB_CLK100,
    SYSCTL_CLK_DPHY_TEST_CLK,
    SYSCTL_CLK_SPI2AXI_ACLK,

    /* Clock-tree nodes from MAIX3时钟复位V1.0. These are topology names
     * without another programmable CMU field; the register-backed nodes above
     * remain the control points. */
    SYSCTL_CLK_TREE_START = 2000,
    SYSCTL_CLK_CPU1_CORE_CLK_ROOT,
    SYSCTL_CLK_CPU1_CORE_CLK,
    SYSCTL_CLK_NOC_CPU1_CLK,
    SYSCTL_CLK_DDRC_P0_CLK,
    SYSCTL_CLK_CPU0_CORE_CLK,
    SYSCTL_CLK_NOC_CPU0_CLK,
    SYSCTL_CLK_DDRC_P4_CLK,
    SYSCTL_CLK_NOC_AI_ACLK,
    SYSCTL_CLK_DDRC_P3_CLK,
    SYSCTL_CLK_VPU_CLK_ROOT,
    SYSCTL_CLK_NOC_VPU_ACLK,
    SYSCTL_CLK_DDRC_P2_CLK,
    SYSCTL_CLK_NOC_VPU_CFG_CLK,
    SYSCTL_CLK_NOC_PMU_PCLK,
    SYSCTL_CLK_NOC_HS_SUB_HCLK,
    SYSCTL_CLK_SD_ACLK,
    SYSCTL_CLK_NOC_HS_QSPI_ACLK,
    SYSCTL_CLK_NOC_LS_PCLK,
    SYSCTL_CLK_NOC_SHRM_PCLK,
    SYSCTL_CLK_NOC_DDRC_PCLK,
    SYSCTL_CLK_DDRC_P1_CLK,
    SYSCTL_CLK_NOC_ISP_HCLK,
    SYSCTL_CLK_NOC_ISP_ACLK,
    SYSCTL_CLK_NOC_DPU_ACLK,
    SYSCTL_CLK_NOC_DPU_PCLK,
    SYSCTL_CLK_NOC_DISP_HCLK,
    SYSCTL_CLK_NOC_DISP_ACLK,
    SYSCTL_CLK_NOC_SPI2AXI_ACLK,

    /* Additional exact clock-tree names from MAIX3时钟复位V1.0. */
    SYSCTL_CLK_NOC_HS_OSPI_ACLK,
    SYSCTL_CLK_NOC_SEC_ACLK,
    SYSCTL_CLK_NOC_SEC_PCLK,
    SYSCTL_CLK_NODE_MAX,
}sysctl_clk_node_e;

typedef sysctl_clk_node_e sysctl_media_clk_node_e;
typedef sysctl_clk_t sysctl_media_clk_t;

typedef enum
{
    SYSCTL_CLK_MUL_CHANGEABLE = 0,
    SYSCTL_CLK_DIV_CHANGEABLE = 1,
    SYSCTL_CLK_MUL_DIV_CHANGEABLE = 2,
}sysctl_media_clk_mul_div_methord_e;

// typedef struct sysctl_attr {
//     sysctl_clk_node_e clk;
//     volatile uint32_t sysctl_readonly;
//     volatile uint32_t sysctl_readwrite;
//     volatile uint32_t sysctl_noreadwrite;
// } sysctl_clk_attr_t;

// typedef struct sysctl_attr {
//     bool sysctl_read;
//     bool sysctl_write;
// } sysctl_clk_attr_t;

#define SYSCTL_READ_ENABLE (1 << 0)
#define SYSCTL_READ_DISABLE (0 << 0)
#define SYSCTL_WRITE_ENABLE (1 << 1)
#define SYSCTL_WRITE_DISABLE (0 << 1)
// typedef enum
// {
//     ERROR_RO = 0,
//     ERROR_RW,
//     ERROR_NOTRW,
// } sysctl_err_t;

/**********************************************************************************************************************
*
* 针对根时钟的API。24M、PLL0-3, 这5个时钟是根时钟
* 这里假设大核对根时钟具有读写权限，因此在这些API中没有判断clk的属性，因为这些API只针对根时钟。
*
**********************************************************************************************************************/
/* 获取锁相环 bypass状态, 如果是bypas，则锁相环输出的是24M OSC时钟 */
bool sysctl_boot_get_root_clk_bypass(sysctl_clk_node_e clk);
void sysctl_boot_set_root_clk_bypass(sysctl_clk_node_e clk, bool enable);

/* Enable pll, enable 24M clock&pll */
/* 打开或者关闭 root时钟 针对锁相环和24M时钟 */
bool sysctl_boot_get_root_clk_en(sysctl_clk_node_e clk);
void sysctl_boot_set_root_clk_en(sysctl_clk_node_e clk, bool enable);
/* Trigger PLL initialization and wait until both lock and PLL_READY. */
bool sysctl_boot_start_root_clk(sysctl_clk_node_e clk);
bool sysctl_boot_set_root_clk_pwroff(sysctl_clk_node_e clk);

/* 获取锁相环锁定状态 */
bool sysctl_boot_get_root_clk_lock(sysctl_clk_node_e clk);

/* 获取root时钟频率 */
uint32_t sysctl_boot_get_root_clk_freq(sysctl_clk_node_e clk);

/* 设置锁相环时钟频率 计算公式 pll_out_freq = (double)OSC_CLOCK_FREQ_24M * (double)(fbdiv+1) / (double)(refdiv+1) / (double)(outdiv+1) */
bool sysctl_boot_set_root_clk_freq(sysctl_clk_node_e clk, uint32_t fbdiv, uint32_t refdiv, uint32_t outdiv, uint32_t bwadj);

/**********************************************************************************************************************
*
* 针对时钟树中trunk和leaf节点的API。即，除了5个根时钟之外的其它时钟。
*
**********************************************************************************************************************/
/* 设置时钟数上叶子节点时钟源, 请根据时钟树来设置, 很多时钟节点只有一个时钟源，因此设置会返回false */
bool sysctl_clk_set_leaf_parent(sysctl_clk_node_e leaf, sysctl_clk_node_e parent);
/* 获取时钟树上叶子节点时钟源 */
sysctl_clk_node_e sysctl_clk_get_leaf_parent(sysctl_clk_node_e leaf);

/* 设置时钟节点enable,注意:只设置本时钟节点的enable，不会设置上游时钟的enable。
   同linux kernel的区别: linux kernel clock framework 会自动设置上游时钟的enable，测试代码没有kernel框架，因此只设置本节点时钟的enable */
void sysctl_clk_set_leaf_en(sysctl_clk_node_e leaf, bool enable);

/* Enable or disable the display-domain VG-Lite clock gate. */
void sysctl_clk_set_disp_gpu_en(bool enable);

/* 获取本时钟节点的enable状态 */
bool sysctl_clk_get_leaf_en(sysctl_clk_node_e leaf);

/* 设置本时钟节点的分频系数 */
bool sysctl_clk_set_leaf_div(sysctl_clk_node_e leaf, uint32_t numerator, uint32_t denominator);

/* 获取本时钟节点的分频系数 */
double sysctl_clk_get_leaf_div(sysctl_clk_node_e leaf);

// /* 设置本时钟节点的相位 */
// bool sysctl_clk_set_phase(sysctl_clk_node_e leaf, uint32_t degree);

// /* 获取本时钟节点的相位 */
// uint32_t sysctl_clk_get_phase(sysctl_clk_node_e leaf);

/* calc clock freqency */
/* 计算当前时钟节点的频率, 这个API会搜索整个时钟路径，从时钟源开始计算每一级的分频，最终得出当前时钟频率 */
uint32_t sysctl_clk_get_leaf_freq(sysctl_clk_node_e leaf);

bool sysctl_media_clk_is_node(sysctl_media_clk_node_e node);
bool sysctl_media_clk_set_leaf_parent(sysctl_media_clk_node_e leaf, sysctl_media_clk_node_e parent);
sysctl_media_clk_node_e sysctl_media_clk_get_leaf_parent(sysctl_media_clk_node_e leaf);
void sysctl_media_clk_set_leaf_en(sysctl_media_clk_node_e leaf, bool enable);
int sysctl_media_clk_get_leaf(sysctl_media_clk_node_e leaf);
int sysctl_media_clk_put_leaf(sysctl_media_clk_node_e leaf);
bool sysctl_media_clk_get_leaf_en(sysctl_media_clk_node_e leaf);
bool sysctl_media_clk_set_leaf_div(sysctl_media_clk_node_e leaf, uint32_t numerator, uint32_t denominator);
double sysctl_media_clk_get_leaf_div(sysctl_media_clk_node_e leaf);
uint32_t sysctl_media_clk_get_leaf_freq(sysctl_media_clk_node_e leaf);
int sysctl_media_clk_find_approximate(sysctl_media_clk_node_e leaf,
                                      uint32_t mul_min,
                                      uint32_t mul_max,
                                      uint32_t div_min,
                                      uint32_t div_max,
                                      sysctl_media_clk_mul_div_methord_e method,
                                      unsigned long rate,
                                      unsigned long parent_rate,
                                      uint32_t *div,
                                      uint32_t *mul);
/* Caller must serialize raw mux/divider updates. */
bool sysctl_media_clk_set_leaf_parent_div_raw(sysctl_media_clk_node_e leaf, sysctl_media_clk_node_e parent, uint32_t numerator, uint32_t denominator);
bool sysctl_media_clk_set_leaf_parent_div(sysctl_media_clk_node_e leaf, sysctl_media_clk_node_e parent, uint32_t numerator, uint32_t denominator);
void sysctl_media_clk_set_leaf_en_multi(sysctl_media_clk_node_e leaf, bool enable);

#endif
