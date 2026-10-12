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
#include <stdbool.h>
#include "sysctl_rst.h"
#include "ioremap.h"
#include "board.h"
#include <rthw.h>

#include <riscv_io.h>

/* created by yangfan */
typedef enum
{
    W1C = 1<<0,
    W1T = 1<<1,
    WE = 1<<2, //write enable
    RWSC = 1<<3,
    RW_0 = 1<<4,
    RW_1 = 1<<5,
} reset_type_t;

typedef struct {
    sysctl_reset_e reset;
    uint32_t type;
    uint8_t reg_offset;
    uint8_t reset_bit;
    uint8_t done_bit;
    uint32_t mask_rw;
} reset_t;

volatile sysctl_rst_t* sysctl_rst = (volatile sysctl_rst_t*)SYSCTL_RST_BASE_ADDR;

static const reset_t k230_reset[SYSCTL_RESET_MAX] = {
    [SYSCTL_RESET_CPU0_CORE] = {SYSCTL_RESET_CPU0_CORE, WE|W1T|W1C, 0x4, 0,  12, 0x0},
    [SYSCTL_RESET_CPU0_TDI] = {SYSCTL_RESET_CPU0_TDI, WE|W1T|W1C, 0x4, 1,  14, 0x0},
    [SYSCTL_RESET_CPU0_APB] = {SYSCTL_RESET_CPU0_APB, WE|W1T|W1C, 0x4, 2,  13, 0x0},
    [SYSCTL_RESET_CPU1_APB] = {SYSCTL_RESET_CPU1_APB, WE|W1T|W1C, 0x4, 3,  13, 0x0},
    [SYSCTL_RESET_CPU0_FLUSH] = {SYSCTL_RESET_CPU0_FLUSH, WE|RWSC, 0x4, 4,  4, 0x0},
    [SYSCTL_RESET_CPU1_CORE] = {SYSCTL_RESET_CPU1_CORE, WE|RW_1|W1C, 0xc, 0,  12, 0x1},
    [SYSCTL_RESET_CPU1_FLUSH] = {SYSCTL_RESET_CPU1_FLUSH, WE|RWSC, 0xc, 4,  4, 0x1},
    [SYSCTL_RESET_AI] = {SYSCTL_RESET_AI, W1T|W1C, 0x14, 0,  31, 0x0},
    [SYSCTL_RESET_VPU] = {SYSCTL_RESET_VPU, W1T|W1C, 0x1c, 0,  31, 0x0},
    [SYSCTL_RESET_TIMER0] = {SYSCTL_RESET_TIMER0, RW_0, 0x20, 0,  0, 0xff0ff},
    [SYSCTL_RESET_TIMER1] = {SYSCTL_RESET_TIMER1, RW_0, 0x20, 1,  0, 0xff0ff},
    [SYSCTL_RESET_TIMER2] = {SYSCTL_RESET_TIMER2, RW_0, 0x20, 2,  0, 0xff0ff},
    [SYSCTL_RESET_TIMER3] = {SYSCTL_RESET_TIMER3, RW_0, 0x20, 3,  0, 0xff0ff},
    [SYSCTL_RESET_TIMER4] = {SYSCTL_RESET_TIMER4, RW_0, 0x20, 4,  0, 0xff0ff},
    [SYSCTL_RESET_TIMER5] = {SYSCTL_RESET_TIMER5, RW_0, 0x20, 5,  0, 0xff0ff},
    [SYSCTL_RESET_TIMER_APB] = {SYSCTL_RESET_TIMER_APB, RW_0, 0x20, 6,  0, 0xff0ff},
    [SYSCTL_RESET_HDI] = {SYSCTL_RESET_HDI, RW_0, 0x20, 7,  0, 0xff0ff},
    [SYSCTL_RESET_WDT0] = {SYSCTL_RESET_WDT0, RW_0, 0x20, 12,  0, 0xff0ff},
    [SYSCTL_RESET_WDT1] = {SYSCTL_RESET_WDT1, RW_0, 0x20, 13,  0, 0xff0ff},
    [SYSCTL_RESET_WDT0_APB] = {SYSCTL_RESET_WDT0_APB, RW_0, 0x20, 14,  0, 0xff0ff},
    [SYSCTL_RESET_WDT1_APB] = {SYSCTL_RESET_WDT1_APB, RW_0, 0x20, 15,  0, 0xff0ff},
    [SYSCTL_RESET_TS_APB] = {SYSCTL_RESET_TS_APB, RW_0, 0x20, 16,  0, 0xff0ff},
    [SYSCTL_RESET_MAILBOX] = {SYSCTL_RESET_MAILBOX, RW_0, 0x20, 17,  0, 0xff0ff},
    [SYSCTL_RESET_STC] = {SYSCTL_RESET_STC, RW_0, 0x20, 18,  0, 0xff0ff},
    [SYSCTL_RESET_PMU] = {SYSCTL_RESET_PMU, RW_0, 0x20, 19,  0, 0xff0ff},
    [SYSCTL_RESET_LS_APB] = {SYSCTL_RESET_LS_APB, RW_0, 0x24, 0,  0, 0x7e7fff},
    [SYSCTL_RESET_UART0] = {SYSCTL_RESET_UART0, RW_0, 0x24, 1,  0, 0x7e7fff},
    [SYSCTL_RESET_UART1] = {SYSCTL_RESET_UART1, RW_0, 0x24, 2,  0, 0x7e7fff},
    [SYSCTL_RESET_UART2] = {SYSCTL_RESET_UART2, RW_0, 0x24, 3,  0, 0x7e7fff},
    [SYSCTL_RESET_UART3] = {SYSCTL_RESET_UART3, RW_0, 0x24, 4,  0, 0x7e7fff},
    [SYSCTL_RESET_UART4] = {SYSCTL_RESET_UART4, RW_0, 0x24, 5,  0, 0x7e7fff},
    [SYSCTL_RESET_I2C0] = {SYSCTL_RESET_I2C0, RW_0, 0x24, 6,  0, 0x7e7fff},
    [SYSCTL_RESET_I2C1] = {SYSCTL_RESET_I2C1, RW_0, 0x24, 7,  0, 0x7e7fff},
    [SYSCTL_RESET_I2C2] = {SYSCTL_RESET_I2C2, RW_0, 0x24, 8,  0, 0x7e7fff},
    [SYSCTL_RESET_I2C3] = {SYSCTL_RESET_I2C3, RW_0, 0x24, 9,  0, 0x7e7fff},
    [SYSCTL_RESET_I2C4] = {SYSCTL_RESET_I2C4, RW_0, 0x24, 10,  0, 0x7e7fff},
    [SYSCTL_RESET_JAMLINK0_APB] = {SYSCTL_RESET_JAMLINK0_APB, RW_0, 0x24, 11,  0, 0x7e7fff},
    [SYSCTL_RESET_JAMLINK1_APB] = {SYSCTL_RESET_JAMLINK1_APB, RW_0, 0x24, 12,  0, 0x7e7fff},
    [SYSCTL_RESET_JAMLINK2_APB] = {SYSCTL_RESET_JAMLINK2_APB, RW_0, 0x24, 13,  0, 0x7e7fff},
    [SYSCTL_RESET_JAMLINK3_APB] = {SYSCTL_RESET_JAMLINK3_APB, RW_0, 0x24, 14,  0, 0x7e7fff},
    [SYSCTL_RESET_CODEC_APB] = {SYSCTL_RESET_CODEC_APB, RW_0, 0x24, 17,  0, 0x7e7fff},
    [SYSCTL_RESET_GPIO_DB] = {SYSCTL_RESET_GPIO_DB, RW_0, 0x24, 18,  0, 0x7e7fff},
    [SYSCTL_RESET_GPIO_APB] = {SYSCTL_RESET_GPIO_APB, RW_0, 0x24, 19,  0, 0x7e7fff},
    [SYSCTL_RESET_ADC] = {SYSCTL_RESET_ADC, RW_0, 0x24, 20,  0, 0x7e7fff},
    [SYSCTL_RESET_ADC_APB] = {SYSCTL_RESET_ADC_APB, RW_0, 0x24, 21,  0, 0x7e7fff},
    [SYSCTL_RESET_PWM_APB] = {SYSCTL_RESET_PWM_APB, RW_0, 0x24, 22,  0, 0x7e7fff},
    [SYSCTL_RESET_HS] = {SYSCTL_RESET_HS, W1T|W1C, 0x2c, 0,  4, 0x0},
    [SYSCTL_RESET_HS_AHB] = {SYSCTL_RESET_HS_AHB, W1T|W1C, 0x2c, 1,  5, 0x0},
    [SYSCTL_RESET_SDIO0] = {SYSCTL_RESET_SDIO0, W1T|W1C, 0x34, 0,  28, 0x0},
    [SYSCTL_RESET_SDIO1] = {SYSCTL_RESET_SDIO1, W1T|W1C, 0x34, 1,  29, 0x0},
    [SYSCTL_RESET_SDIO_AXI] = {SYSCTL_RESET_SDIO_AXI, W1T|W1C, 0x34, 2,  30, 0x0},
    [SYSCTL_RESET_USB0] = {SYSCTL_RESET_USB0, W1T|W1C, 0x3c, 0,  28, 0x0},
    [SYSCTL_RESET_USB0_AHB] = {SYSCTL_RESET_USB0_AHB, W1T|W1C, 0x3c, 0,  30, 0x0},
    [SYSCTL_RESET_USB1] = {SYSCTL_RESET_USB1, W1T|W1C, 0x3c, 1,  29, 0x0},
    [SYSCTL_RESET_USB1_AHB] = {SYSCTL_RESET_USB1_AHB, W1T|W1C, 0x3c, 1,  31, 0x0},
    [SYSCTL_RESET_SPI0] = {SYSCTL_RESET_SPI0, W1T|W1C, 0x44, 0,  28, 0x0},
    [SYSCTL_RESET_SPI1] = {SYSCTL_RESET_SPI1, W1T|W1C, 0x44, 1,  29, 0x0},
    [SYSCTL_RESET_SPI2] = {SYSCTL_RESET_SPI2, W1T|W1C, 0x44, 2,  30, 0x0},
    [SYSCTL_RESET_SEC] = {SYSCTL_RESET_SEC, W1T|W1C, 0x4c, 0,  31, 0x0},
    [SYSCTL_RESET_PDMA] = {SYSCTL_RESET_PDMA, W1T|W1C, 0x54, 0,  28, 0x0},
    [SYSCTL_RESET_SDMA] = {SYSCTL_RESET_SDMA, W1T|W1C, 0x54, 1,  29, 0x0},
    [SYSCTL_RESET_DECOMPRESS] = {SYSCTL_RESET_DECOMPRESS, W1T|W1C, 0x5c, 0,  31, 0x0},
    [SYSCTL_RESET_SRAM] = {SYSCTL_RESET_SRAM, W1T|W1C, 0x64, 0,  28, 0x2},
    [SYSCTL_RESET_SHRM_APB] = {SYSCTL_RESET_SHRM_APB, RW_0, 0x64, 1,  0, 0x2},
    [SYSCTL_RESET_SHRM_AXIM] = {SYSCTL_RESET_SHRM_AXIM, W1T|W1C, 0x64, 2,  30, 0x2},
    [SYSCTL_RESET_SHRM_AXIS] = {SYSCTL_RESET_SHRM_AXIS, W1T|W1C, 0x64, 3,  31, 0x2},
    [SYSCTL_RESET_NONAI2D] = {SYSCTL_RESET_NONAI2D, W1T|W1C, 0x6c, 0,  31, 0x0},
    [SYSCTL_RESET_MCTL] = {SYSCTL_RESET_MCTL, W1T|W1C, 0x74, 0,  31, 0x0},
    [SYSCTL_RESET_CSI0_APB] = {SYSCTL_RESET_CSI0_APB, RW_0, 0x80, 0,  0, 0x39f},
    [SYSCTL_RESET_CSI1_APB] = {SYSCTL_RESET_CSI1_APB, RW_0, 0x80, 1,  0, 0x39f},
    [SYSCTL_RESET_CSI2_APB] = {SYSCTL_RESET_CSI2_APB, RW_0, 0x80, 2,  0, 0x39f},
    [SYSCTL_RESET_CSI_DPHY_APB] = {SYSCTL_RESET_CSI_DPHY_APB, RW_0, 0x80, 3,  0, 0x39f},
    [SYSCTL_RESET_ISP_AHB] = {SYSCTL_RESET_ISP_AHB, RW_0, 0x80, 4,  0, 0x39f},
    [SYSCTL_RESET_ISP_DW] = {SYSCTL_RESET_ISP_DW, W1T|W1C, 0x80, 5,  28, 0x39f},
    [SYSCTL_RESET_ISP] = {SYSCTL_RESET_ISP, W1T|W1C, 0x80, 6,  29, 0x39f},
    [SYSCTL_RESET_M0] = {SYSCTL_RESET_M0, RW_0, 0x80, 7,  0, 0x39f},
    [SYSCTL_RESET_M1] = {SYSCTL_RESET_M1, RW_0, 0x80, 8,  0, 0x39f},
    [SYSCTL_RESET_M2] = {SYSCTL_RESET_M2, RW_0, 0x80, 9,  0, 0x39f},
    [SYSCTL_RESET_DPU] = {SYSCTL_RESET_DPU, W1T|W1C, 0x88, 0,  31, 0x0},
    [SYSCTL_RESET_DISP] = {SYSCTL_RESET_DISP, W1T|W1C, 0x90, 0,  31, 0x0},
    [SYSCTL_RESET_GPU] = {SYSCTL_RESET_GPU, W1T|W1C, 0x98, 0,  31, 0x0},
    [SYSCTL_RESET_AUDIO] = {SYSCTL_RESET_AUDIO, W1T|W1C, 0xa4, 0,  31, 0x0},
    [SYSCTL_RESET_SPI2AXI] = {SYSCTL_RESET_SPI2AXI, RW_0, 0xa8, 0,  0, 0x1},
};

bool sysctl_reset(sysctl_reset_e reset)
{
    if ((unsigned int)reset >= SYSCTL_RESET_MAX)
        return false;
    if (k230_reset[reset].reset != reset)
        return false;

    uint32_t type = k230_reset[reset].type;
    uint8_t reg_offset = k230_reset[reset].reg_offset;
    uint8_t reset_bit = k230_reset[reset].reset_bit;
    uint8_t done_bit = k230_reset[reset].done_bit;
    uint32_t mask_rw = k230_reset[reset].mask_rw;
    uint32_t data = 0;

    /* loop __UINT16_MAX__ take 369 us in k230 */
    volatile uint32_t delay = ((__UINT16_MAX__ << 1) + __UINT16_MAX__);

    rt_base_t level;

    volatile uint32_t *reset_reg = (volatile uint32_t *)sysctl_rst + reg_offset/4;

    level = rt_hw_interrupt_disable();
    /* clear done bit */
    if(type & W1C)
    {
        data = *reset_reg & mask_rw;

        if((reset == SYSCTL_RESET_USB0) || (reset == SYSCTL_RESET_USB1)) {
            data |= ((1 << done_bit) | (1 << (done_bit + 2)));
        } else {
            data |= (1 << done_bit);
        }
        if(type & WE)
        {
            data |= (1 << (done_bit + 16));  /* write enable */
        }
        *reset_reg = data;
    }
    /* set reset bit */
    data = *reset_reg & mask_rw;

    if((type & W1T) || (type & RWSC) || (type & RW_1))
    {
        data |= (1 << reset_bit);
    }
    else if(type & RW_0)
    {
        data &= ~(1 << reset_bit);
    }
    if(type & WE)
    {
        data |= (1 << (reset_bit + 16));  /* write enable */
    }
    *reset_reg = data;
    rt_hw_interrupt_enable(level);

    rt_thread_delay(1);
    
    /* clear reset bit */
    if((type & RW_0) || (type & RW_1))
    {
        level = rt_hw_interrupt_disable();
        data = *reset_reg & mask_rw;

        if(type & RW_0) data |= (1 << reset_bit);
        else if(type & RW_1) data &= ~(1 << reset_bit);

        if(type & WE)
        {
            data |= (1 << (reset_bit + 16));  /* write enable */
        }
        *reset_reg = data;
        rt_hw_interrupt_enable(level);
    }

    /* check done bit */
    if(type & W1C)
    {
        if((reset == SYSCTL_RESET_USB0) || (reset == SYSCTL_RESET_USB1)) {
            uint32_t done;

            done = (1 << done_bit) | (1 << (done_bit + 2));
            while((done != (readl(reset_reg) & done)) && (--delay));

            if((readl(reset_reg) & done) == done) {
                return true;
            }

            return false;
        } else {
            while((0x00 == (readl(reset_reg) & (1 << done_bit))) && (--delay));

            if(readl(reset_reg) & (1 << done_bit)) {
                return true;
            }

            return false;
        }
    }

    if(type & RWSC)
    {
        while((0x00 != (readl(reset_reg) & (1 << done_bit))) && (--delay));

        if((readl(reset_reg) & (1 << done_bit)) == 0) {
            return true;
        }

        return false;
    }

    return true;
}

/* time = timx(x=0,1,2) * 0.25us, please see sys_ctl_reg.xlsx
   tim0/1/2 for 3 step reset(clock toggle) ---> 查看《MAIX3系统控制设计文档V1.0》
   tim1/2 for 2 step reset(clock gate) ---> 查看《MAIX3系统控制设计文档V1.0》
*/
#if 0
bool sysctl_set_reset_time(sysctl_reset_time_e reset, uint32_t tim0, uint32_t tim1, uint32_t tim2)
{
    switch(reset)
    {
        case SYSCTL_RESET_TIME_CPU0:
        {
            if((tim1 > 0xF) || (tim2 > 0xF))
            {
                return false;
            }
            else
            {
                sysctl_rst->cpu0_rst_tim |= ((tim1 << 12) | (tim2 << 20));
                return true;
            }
        }
        case SYSCTL_RESET_TIME_CPU0_APB:
        {
            if((tim1 > 0xF) || (tim2 > 0xF))
            {
                return false;
            }
            else
            {
                sysctl_rst->cpu0_rst_tim |= ((tim1 << 4) | (tim2 << 8));
                return true;
            }
        }
        case SYSCTL_RESET_TIME_CPU1:
        {
            if((tim1 > 0xF) || (tim2 > 0xF))
            {
                return false;
            }
            else
            {
                sysctl_rst->cpu1_rst_tim |= ((tim1 << 12) | (tim2 << 16));
                return true;
            }
        }
        case SYSCTL_RESET_TIME_CPU1_APB:
        {
            if((tim1 > 0xF) || (tim2 > 0xF))
            {
                return false;
            }
            else
            {
                sysctl_rst->cpu1_rst_tim |= ((tim1 << 4) | (tim2 << 8));
                return true;
            }
        }
        case SYSCTL_RESET_TIME_AI:
        {
            if((tim1 > 0xF) || (tim2 > 0xF))
            {
                return false;
            }
            else
            {
                sysctl_rst->ai_rst_tim |= ((tim1 << 4) | (tim2 << 8));
                return true;
            }
        }
        case SYSCTL_RESET_TIME_VPU:
        {
            if((tim1 > 0xF) || (tim2 > 0xF))
            {
                return false;
            }
            else
            {
                sysctl_rst->vpu_rst_tim |= ((tim1 << 4) | (tim2 << 8));
                return true;
            }
        }
        case SYSCTL_RESET_TIME_HS_HCLK:
        {
            if((tim1 > 0x1F) || (tim2 > 0xF))
            {
                return false;
            }
            else
            {
                sysctl_rst->hisys_hclk_tim |= ((tim1 << 8) | (tim2 << 16));
                return true;
            }
        }
        case SYSCTL_RESET_TIME_SDCTL:
        {
            if((tim1 > 0x1F) || (tim2 > 0xF))
            {
                return false;
            }
            else
            {
                sysctl_rst->sdctl_rst_tim |= ((tim1 << 8) | (tim2 << 16));
                return true;
            }
        }
        case SYSCTL_RESET_TIME_USB:
        {
            if((tim1 > 0xF) || (tim2 > 0xF))
            {
                return false;
            }
            else
            {
                sysctl_rst->usb_rst_tim |= ((tim1 << 0) | (tim2 << 4));
                return true;
            }
        }
        case SYSCTL_RESET_TIME_USB_AHB:
        {
            if((tim1 > 0xF) || (tim2 > 0xF))
            {
                return false;
            }
            else
            {
                sysctl_rst->usb_rst_tim |= ((tim1 << 8) | (tim2 << 16));
                return true;
            }
        }
        case SYSCTL_RESET_TIME_SPI:
        {
            if((tim1 > 0x3F) || (tim2 > 0xF))
            {
                return false;
            }
            else
            {
                sysctl_rst->spi_rst_tim |= ((tim1 << 8) | (tim2 << 16));
                return true;
            }
        }
        case SYSCTL_RESET_TIME_SEC_SYS:
        {
            if((tim1 > 0xFF) || (tim2 > 0xF))
            {
                return false;
            }
            else
            {
                sysctl_rst->sec_sys_rst_tim |= ((tim1 << 8) | (tim2 << 16));
                return true;
            }
        }
        case SYSCTL_RESET_TIME_DMAC:
        {
            if((tim1 > 0x7) || (tim2 > 0x7))
            {
                return false;
            }
            else
            {
                sysctl_rst->dmac_rst_tim |= ((tim1 << 8) | (tim2 << 16));
                return true;
            }
        }
        case SYSCTL_RESET_TIME_DECOMPRESS:
        {
            if((tim1 > 0x7) || (tim2 > 0x7))
            {
                return false;
            }
            else
            {
                sysctl_rst->decompress_rst_tim |= ((tim1 << 8) | (tim2 << 16));
                return true;
            }
        }
        case SYSCTL_RESET_TIME_SRAM:
        {
            if((tim1 > 0xF) || (tim2 > 0xF))
            {
                return false;
            }
            else
            {
                sysctl_rst->sram_rst_tim |= ((tim1 << 8) | (tim2 << 16));
                return true;
            }
        }
        case SYSCTL_RESET_TIME_NONAI2D:
        {
            if((tim1 > 0xF) || (tim2 > 0xF))
            {
                return false;
            }
            else
            {
                sysctl_rst->nonai2d_rst_tim |= ((tim1 << 8) | (tim2 << 16));
                return true;
            }
        }
        case SYSCTL_RESET_TIME_MCTL:
        {
            if(tim0 > 0xF)
            {
                return false;
            }
            else
            {
                sysctl_rst->mctl_rst_tim |= (tim0 << 0);
                return true;
            }
        }
        case SYSCTL_RESET_TIME_ISP:
        {
            if((tim0 > 0xFF) || (tim1 > 0xF) || (tim2 > 0xF))
            {
                return false;
            }
            else
            {
                sysctl_rst->isp_rst_tim |= ((tim0 << 0) | (tim1 << 8) | (tim2 << 16));
                return true;
            }
        }
        case SYSCTL_RESET_TIME_ISP_DW:
        {
            if((tim0 > 0xFF) || (tim1 > 0xF) || (tim2 > 0xF))
            {
                return false;
            }
            else
            {
                sysctl_rst->isp_dw_rst_tim |= ((tim0 << 0) | (tim1 << 8) | (tim2 << 16));
                return true;
            }
        }
        case SYSCTL_RESET_TIME_DPU:
        {
            if((tim1 > 0xF) || (tim2 > 0xF))
            {
                return false;
            }
            else
            {
                sysctl_rst->dpu_rst_tim |= ((tim1 << 8) | (tim2 << 16));
                return true;
            }
        }
        case SYSCTL_RESET_TIME_DISP_SYS:
        {
            if((tim0 > 0xFF) || (tim1 > 0xFF) || (tim2 > 0xF))
            {
                return false;
            }
            else
            {
                sysctl_rst->disp_sys_rst_tim |= ((tim0 << 0) | (tim1 << 8) | (tim2 << 16));
                return true;
            }
        }
        case SYSCTL_RESET_TIME_V2P5D_SYS:
        {
            if((tim0 > 0xFF) || (tim1 > 0xFF) || (tim2 > 0xF))
            {
                return false;
            }
            else
            {
                sysctl_rst->v2p5d_sys_rst_tim |= ((tim0 << 0) | (tim1 << 8) | (tim2 << 16));
                return true;
            }
        }
        case SYSCTL_RESET_TIME_AUDIO:
        {
            if((tim1 > 0xF) || (tim2 > 0xF))
            {
                return false;
            }
            else
            {
                sysctl_rst->audio_rst_tim |= ((tim1 << 4) | (tim2 << 8));
                return true;
            }
        }

        default:
            return false;
    }
}
#endif
bool sysctl_set_reset_time(sysctl_reset_time_e reset, uint32_t tim0, uint32_t tim1, uint32_t tim2)
{
    volatile uint32_t ret;

    switch(reset)
    {
        case SYSCTL_RESET_TIME_CPU0:
        {
            if((tim1 > 0xF) || (tim2 > 0xF))
            {
                return false;
            }
            else
            {
                ret = sysctl_rst->cpu0_rst_tim;
                ret &= 0xf0000fff;
                sysctl_rst->cpu0_rst_tim = ret | ((tim1 << 12) | (tim2 << 20));
                return true;
            }
        }
        case SYSCTL_RESET_TIME_CPU0_APB:
        {
            if((tim1 > 0xF) || (tim2 > 0xF))
            {
                return false;
            }
            else
            {
                ret = sysctl_rst->cpu0_rst_tim;
                ret &= 0xfffff00f;
                sysctl_rst->cpu0_rst_tim = ret | ((tim1 << 4) | (tim2 << 8));
                return true;
            }
        }
        case SYSCTL_RESET_TIME_CPU1:
        {
            if((tim1 > 0xF) || (tim2 > 0xF))
            {
                return false;
            }
            else
            {
                ret = sysctl_rst->cpu1_rst_tim;
                ret &= 0xfff00fff;
                sysctl_rst->cpu1_rst_tim = ret | ((tim1 << 12) | (tim2 << 16));
                return true;
            }
        }
        case SYSCTL_RESET_TIME_CPU1_APB:
        {
            if((tim1 > 0xF) || (tim2 > 0xF))
            {
                return false;
            }
            else
            {
                ret = sysctl_rst->cpu1_rst_tim;
                ret &= 0xfffff00f;
                sysctl_rst->cpu1_rst_tim = ret | ((tim1 << 4) | (tim2 << 8));
                return true;
            }
        }
        case SYSCTL_RESET_TIME_AI:
        {
            if((tim1 > 0xF) || (tim2 > 0xF))
            {
                return false;
            }
            else
            {
                ret = sysctl_rst->ai_rst_tim;
                ret &= 0xfffff00f;
                sysctl_rst->ai_rst_tim = ret | ((tim1 << 4) | (tim2 << 8));
                return true;
            }
        }
        case SYSCTL_RESET_TIME_VPU:
        {
            if((tim1 > 0xF) || (tim2 > 0xF))
            {
                return false;
            }
            else
            {
                ret = sysctl_rst->vpu_rst_tim;
                ret &= 0xfffff00f;
                sysctl_rst->vpu_rst_tim = ret | ((tim1 << 4) | (tim2 << 8));
                return true;
            }
        }
        case SYSCTL_RESET_TIME_HS_HCLK:
        {
            if((tim1 > 0x1F) || (tim2 > 0xF))
            {
                return false;
            }
            else
            {
                ret = sysctl_rst->hisys_hclk_tim;
                ret &= 0xfff0f0ff;
                sysctl_rst->hisys_hclk_tim = ret | ((tim1 << 8) | (tim2 << 16));
                return true;
            }
        }
        case SYSCTL_RESET_TIME_SDCTL:
        {
            if((tim1 > 0x1F) || (tim2 > 0xF))
            {
                return false;
            }
            else
            {
                ret = sysctl_rst->sdctl_rst_tim;
                ret &= 0xfff0f0ff;
                sysctl_rst->sdctl_rst_tim = ret | ((tim1 << 8) | (tim2 << 16));
                return true;
            }
        }
        case SYSCTL_RESET_TIME_USB:
        {
            if((tim1 > 0xF) || (tim2 > 0xF))
            {
                return false;
            }
            else
            {
                ret = sysctl_rst->usb_rst_tim;
                ret &= 0xffffff00;
                sysctl_rst->usb_rst_tim = ret | ((tim1 << 0) | (tim2 << 4));
                return true;
            }
        }
        case SYSCTL_RESET_TIME_USB_AHB:
        {
            if((tim1 > 0xF) || (tim2 > 0xF))
            {
                return false;
            }
            else
            {
                ret = sysctl_rst->usb_rst_tim;
                ret &= 0xff0000ff;
                sysctl_rst->usb_rst_tim = ret | ((tim1 << 8) | (tim2 << 16));
                return true;
            }
        }
        case SYSCTL_RESET_TIME_SPI:
        {
            if((tim1 > 0x3F) || (tim2 > 0xF))
            {
                return false;
            }
            else
            {
                ret = sysctl_rst->spi_rst_tim;
                ret &= 0xfff0f0ff;
                sysctl_rst->spi_rst_tim = ret | ((tim1 << 8) | (tim2 << 16));
                return true;
            }
        }
        case SYSCTL_RESET_TIME_SEC_SYS:
        {
            if((tim1 > 0xFF) || (tim2 > 0xF))
            {
                return false;
            }
            else
            {
                ret = sysctl_rst->sec_sys_rst_tim;
                ret &= 0xfff0f0ff;
                sysctl_rst->sec_sys_rst_tim = ret | ((tim1 << 8) | (tim2 << 16));
                return true;
            }
        }
        case SYSCTL_RESET_TIME_DMAC:
        {
            if((tim1 > 0x7) || (tim2 > 0x7))
            {
                return false;
            }
            else
            {
                ret = sysctl_rst->dmac_rst_tim;
                ret &= 0xfff0f0ff;
                sysctl_rst->dmac_rst_tim = ret | ((tim1 << 8) | (tim2 << 16));
                return true;
            }
        }
        case SYSCTL_RESET_TIME_DECOMPRESS:
        {
            if((tim1 > 0x7) || (tim2 > 0x7))
            {
                return false;
            }
            else
            {
                ret = sysctl_rst->decompress_rst_tim;
                ret &= 0xfff0f0ff;
                sysctl_rst->decompress_rst_tim = ret | ((tim1 << 8) | (tim2 << 16));
                return true;
            }
        }
        case SYSCTL_RESET_TIME_SRAM:
        {
            if((tim1 > 0xF) || (tim2 > 0xF))
            {
                return false;
            }
            else
            {
                ret = sysctl_rst->sram_rst_tim;
                ret &= 0xfff0f0ff;
                sysctl_rst->sram_rst_tim = ret | ((tim1 << 8) | (tim2 << 16));
                return true;
            }
        }
        case SYSCTL_RESET_TIME_NONAI2D:
        {
            if((tim1 > 0xF) || (tim2 > 0xF))
            {
                return false;
            }
            else
            {
                ret = sysctl_rst->nonai2d_rst_tim;
                ret &= 0xfff0f0ff;
                sysctl_rst->nonai2d_rst_tim = ret | ((tim1 << 8) | (tim2 << 16));
                return true;
            }
        }
        case SYSCTL_RESET_TIME_MCTL:
        {
            if(tim0 > 0xF)
            {
                return false;
            }
            else
            {
                ret = sysctl_rst->mctl_rst_tim;
                ret &= 0xffffffc0;
                sysctl_rst->mctl_rst_tim = ret | (tim0 << 0);
                return true;
            }
        }
        case SYSCTL_RESET_TIME_ISP:
        {
            if((tim0 > 0xFF) || (tim1 > 0xF) || (tim2 > 0xF))
            {
                return false;
            }
            else
            {
                ret = sysctl_rst->isp_rst_tim;
                ret &= 0xfff0f0f0;
                sysctl_rst->isp_rst_tim = ret | ((tim0 << 0) | (tim1 << 8) | (tim2 << 16));
                return true;
            }
        }
        case SYSCTL_RESET_TIME_ISP_DW:
        {
            if((tim0 > 0xFF) || (tim1 > 0xF) || (tim2 > 0xF))
            {
                return false;
            }
            else
            {
                ret = sysctl_rst->isp_dw_rst_tim;
                ret &= 0xfff0f0f0;
                sysctl_rst->isp_dw_rst_tim = ret | ((tim0 << 0) | (tim1 << 8) | (tim2 << 16));
                return true;
            }
        }
        case SYSCTL_RESET_TIME_DPU:
        {
            if((tim1 > 0xF) || (tim2 > 0xF))
            {
                return false;
            }
            else
            {
                ret = sysctl_rst->dpu_rst_tim;
                ret &= 0xfff0f0ff;
                sysctl_rst->dpu_rst_tim = ret | ((tim1 << 8) | (tim2 << 16));
                return true;
            }
        }
        case SYSCTL_RESET_TIME_DISP_SYS:
        {
            if((tim0 > 0xFF) || (tim1 > 0xFF) || (tim2 > 0xF))
            {
                return false;
            }
            else
            {
                ret = sysctl_rst->disp_sys_rst_tim;
                ret &= 0xfff0f0f0;
                sysctl_rst->disp_sys_rst_tim = ret | ((tim0 << 0) | (tim1 << 8) | (tim2 << 16));
                return true;
            }
        }
        case SYSCTL_RESET_TIME_V2P5D_SYS:
        {
            if((tim0 > 0xFF) || (tim1 > 0xFF) || (tim2 > 0xF))
            {
                return false;
            }
            else
            {
                ret = sysctl_rst->v2p5d_sys_rst_tim;
                ret &= 0xfff0f0f0;
                sysctl_rst->v2p5d_sys_rst_tim = ret | ((tim0 << 0) | (tim1 << 8) | (tim2 << 16));
                return true;
            }
        }
        case SYSCTL_RESET_TIME_AUDIO:
        {
            if((tim1 > 0xF) || (tim2 > 0xF))
            {
                return false;
            }
            else
            {
                ret = sysctl_rst->audio_rst_tim;
                ret &= 0xfffff00f;
                sysctl_rst->audio_rst_tim = ret | ((tim1 << 4) | (tim2 << 8));
                return true;
            }
        }

        default:
            return false;
    }
}

int rt_hw_sysctl_rst_init(void)
{
    sysctl_rst = rt_ioremap((void*)RMU_BASE_ADDR, RMU_IO_SIZE);
    if(!sysctl_rst) {
        rt_kprintf("sysctl_rst ioremap error\n");
        return -1;
    }

    return 0;
}
INIT_BOARD_EXPORT(rt_hw_sysctl_rst_init);
