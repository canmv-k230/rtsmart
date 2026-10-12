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
#include <stdio.h>
#include "sysctl_pwr.h"
#include "ioremap.h"
#include "board.h"
#include "k230_pm.h"
#include "tick.h"

/* created by yangfan */

#define SYSCTL_PWR_TRANSITION_TIMEOUT_US 1000

volatile sysctl_pwr_s* sysctl_pwr = (volatile sysctl_pwr_s*)SYSCTL_PWR_BASE_ADDR;

/*****************************************************************************************
*                                SET POWER DOMAIN'S TIME
*   powerdomain:power domain
*   timtype: idleReq_to_idleAck, idleAck_to_idle.....
*   tim_value: ack_to_tim, idle_to_tim......
*****************************************************************************************/

/* Timing fields follow CPU*_PWR_TIM (0x00/0x10) and CPU*_LPI_TIM (0x04/0x14). */
bool sysctl_pwr_set_wfi_tim(volatile uint32_t *reg, uint32_t wfi_tim)
{
    volatile uint32_t *ret = reg;

    if((NULL == reg) || (wfi_tim > 0xFFF))
        return false;
    else
    {
        *ret &= 0x000fffff;
        *reg = (*ret) | (wfi_tim << 20);

        return true;
    }
}

bool sysctl_pwr_set_pwr_su_tim(volatile uint32_t *reg, uint32_t pwr_su_tim)
{
    volatile uint32_t *ret = reg;

    if((NULL == reg) || (pwr_su_tim > 0xFF))
        return false;
    else
    {
        *ret &= 0xfff00fff;
        *reg = (*ret) | (pwr_su_tim << 12);

        return true;
    }
}

bool sysctl_pwr_set_pd_hd_tim(volatile uint32_t *reg, uint32_t pd_hd_tim)
{
    volatile uint32_t *ret = reg;

    if((NULL == reg) || (pd_hd_tim > 0xFF))
        return false;
    else
    {
        *ret &= 0xfffff00f;
        *reg = (*ret) | (pd_hd_tim << 4);

        return true;
    }
}

bool sysctl_pwr_set_iso_su_tim(volatile uint32_t *reg, uint32_t iso_su_tim)
{
    volatile uint32_t *ret = reg;

    if((NULL == reg) || (iso_su_tim > 0xF))
        return false;
    else
    {
        *ret &= 0xfffffff0;
        *reg = (*ret) | (iso_su_tim << 0);

        return true;
    }
}

bool sysctl_pwr_set_idle_hd_tim(volatile uint32_t *reg, uint32_t idle_hd_tim)
{
    volatile uint32_t *ret = reg;

    if((NULL == reg) || (idle_hd_tim > 0x3F))
        return false;
    else
    {
        *ret &= 0xffc0ffff;
        *reg = (*ret) | (idle_hd_tim << 16);

        return true;
    }
}

bool sysctl_pwr_set_idle_to_tim(volatile uint32_t *reg, uint32_t idle_to_tim)
{
    volatile uint32_t *ret = reg;

    if((NULL == reg) || (idle_to_tim > 0x1F))
        return false;
    else
    {
        *ret &= 0xffffe0ff;
        *reg = (*ret) | (idle_to_tim << 8);

        return true;
    }
}

bool sysctl_pwr_set_ack_to_tim(volatile uint32_t *reg, uint32_t ack_to_tim)
{
    volatile uint32_t *ret = reg;

    if((NULL == reg) || (ack_to_tim > 0x1F))
        return false;
    else
    {
        *ret &= 0xffffffe0;
        *reg = (*ret) | (ack_to_tim << 0);

        return true;
    }
}

static bool sysctl_pwr_get_timing_regs(sysctl_pwr_domain_e powerdomain,
    volatile uint32_t **pwr_reg, volatile uint32_t **lpi_reg,
    volatile uint32_t **wfi_reg)
{
    *pwr_reg = NULL;
    *lpi_reg = NULL;
    *wfi_reg = NULL;

    switch (powerdomain) {
    case SYSCTL_PD_CPU0:
        *pwr_reg = (volatile uint32_t *)&sysctl_pwr->cpu0_pwr_tim;
        *lpi_reg = (volatile uint32_t *)&sysctl_pwr->cpu0_lpi_tim;
        *wfi_reg = *pwr_reg;
        break;
    case SYSCTL_PD_CPU1:
        *pwr_reg = (volatile uint32_t *)&sysctl_pwr->cpu1_pwr_tim;
        *lpi_reg = (volatile uint32_t *)&sysctl_pwr->cpu1_lpi_tim;
        *wfi_reg = *pwr_reg;
        break;
    case SYSCTL_PD_AI:
        *pwr_reg = (volatile uint32_t *)&sysctl_pwr->ai_pwr_tim;
        *lpi_reg = (volatile uint32_t *)&sysctl_pwr->ai_lpi_tim;
        break;
    case SYSCTL_PD_DISP:
        *pwr_reg = (volatile uint32_t *)&sysctl_pwr->disp_pwr_tim;
        *lpi_reg = (volatile uint32_t *)&sysctl_pwr->disp_lpi_tim;
        break;
    case SYSCTL_PD_SHRM:
        *pwr_reg = (volatile uint32_t *)&sysctl_pwr->shrm_pwr_tim;
        *lpi_reg = (volatile uint32_t *)&sysctl_pwr->shrm_lpi_tim;
        break;
    case SYSCTL_PD_VPU:
        *pwr_reg = (volatile uint32_t *)&sysctl_pwr->vpu_pwr_tim;
        *lpi_reg = (volatile uint32_t *)&sysctl_pwr->vpu_lpi_tim;
        break;
    case SYSCTL_PD_MCTL:
        *pwr_reg = (volatile uint32_t *)&sysctl_pwr->mctl_pwr_tim0;
        *lpi_reg = (volatile uint32_t *)&sysctl_pwr->mctl_noc_lpi_tim;
        break;
    case SYSCTL_PD_DPU:
        *pwr_reg = (volatile uint32_t *)&sysctl_pwr->dpu_pwr_tim;
        *lpi_reg = (volatile uint32_t *)&sysctl_pwr->dpu_lpi_tim;
        break;
    case SYSCTL_PD_HI:
        *pwr_reg = (volatile uint32_t *)&sysctl_pwr->hi_pwr_tim;
        *lpi_reg = (volatile uint32_t *)&sysctl_pwr->hi_lpi_tim;
        break;
    case SYSCTL_PD_LS:
        *pwr_reg = (volatile uint32_t *)&sysctl_pwr->ls_pwr_tim;
        *lpi_reg = (volatile uint32_t *)&sysctl_pwr->ls_lpi_tim;
        break;
    case SYSCTL_PD_SEC:
        *pwr_reg = (volatile uint32_t *)&sysctl_pwr->sec_pwr_tim;
        *lpi_reg = (volatile uint32_t *)&sysctl_pwr->sec_lpi_tim;
        break;
    case SYSCTL_PD_ISP:
        *pwr_reg = (volatile uint32_t *)&sysctl_pwr->isp_pwr_tim;
        *lpi_reg = (volatile uint32_t *)&sysctl_pwr->isp_lpi_tim;
        break;
    case SYSCTL_PD_PMU:
        *pwr_reg = (volatile uint32_t *)&sysctl_pwr->pmu_pwr_tim;
        *lpi_reg = (volatile uint32_t *)&sysctl_pwr->pmu_lpi_tim;
        break;
    default:
        return false;
    }

    return true;
}

bool sysctl_pwr_set_tim(sysctl_pwr_domain_e powerdomain, sysctl_pwr_tim_e timtype, uint32_t tim_value)
{
    volatile uint32_t *pwr_reg = NULL;
    volatile uint32_t *lpi_reg = NULL;
    volatile uint32_t *wfi_reg = NULL;

    if((powerdomain >= SYSCTL_PD_MAX) || (timtype >= SYSCTL_PWR_MAX_TIM))
        return false;

    if (!sysctl_pwr_get_timing_regs(powerdomain, &pwr_reg, &lpi_reg, &wfi_reg))
        return false;

    switch(timtype)
    {
        case SYSCTL_PWR_WFI_TIM:
            return sysctl_pwr_set_wfi_tim(wfi_reg, tim_value);
        case SYSCTL_PWR_SU_TIM:
            return sysctl_pwr_set_pwr_su_tim(pwr_reg, tim_value);
        case SYSCTL_PWR_PD_HD_TIM:
            return sysctl_pwr_set_pd_hd_tim(pwr_reg, tim_value);
        case SYSCTL_PWR_ISO_SU_TIM:
            return sysctl_pwr_set_iso_su_tim(pwr_reg, tim_value);
        case SYSCTL_PWR_IDLE_HD_TIM:
            return sysctl_pwr_set_idle_hd_tim(lpi_reg, tim_value);
        case SYSCTL_PWR_IDLE_TO_TIM:
            return sysctl_pwr_set_idle_to_tim(lpi_reg, tim_value);
        case SYSCTL_PWR_ACK_TO_TIM:
            return sysctl_pwr_set_ack_to_tim(lpi_reg, tim_value);
        default:
            return false;
    }
}

/*****************************************************************************************
*                                GET POWER DOMAIN'S TIME
*   powerdomain:power domain
*   timtype: idleReq_to_idleAck, idleAck_to_idle.....
*   tim_value: ack_to_tim, idle_to_tim......
*****************************************************************************************/

/* Read the same timing fields in register-map order. */
bool sysctl_pwr_get_wfi_tim(volatile uint32_t *reg, uint32_t *wfi_tim)
{
    if((NULL == reg) || (NULL == wfi_tim))
        return false;

    *wfi_tim = (*reg >> 20) & 0xFFF;
    return true;
}

bool sysctl_pwr_get_pwr_su_tim(volatile uint32_t *reg, uint32_t *pwr_su_tim)
{
    if((NULL == reg) || (NULL == pwr_su_tim))
        return false;

    *pwr_su_tim = ((*reg) >> 12) & 0xFF;
    return true;
}

bool sysctl_pwr_get_pd_hd_tim(volatile uint32_t *reg, uint32_t *pd_hd_tim)
{
    if((NULL == reg) || (NULL == pd_hd_tim))
        return false;

    *pd_hd_tim = ((*reg) >> 4) & 0xFF;
    return true;
}

bool sysctl_pwr_get_iso_su_tim(volatile uint32_t *reg, uint32_t *iso_su_tim)
{
    if((NULL == reg) || (NULL == iso_su_tim))
        return false;

    *iso_su_tim = ((*reg) >> 0) & 0xF;
    return true;
}

bool sysctl_pwr_get_idle_hd_tim(volatile uint32_t *reg, uint32_t *idle_hd_tim)
{
    if((NULL == reg) || (NULL == idle_hd_tim))
        return false;

    *idle_hd_tim = ((*reg) >> 16) & 0x3F;
    return true;
}

bool sysctl_pwr_get_idle_to_tim(volatile uint32_t *reg, uint32_t *idle_to_tim)
{
    if((NULL == reg) || (NULL == idle_to_tim))
        return false;

    *idle_to_tim = ((*reg) >> 8) & 0x1F;
    return true;
}

bool sysctl_pwr_get_ack_to_tim(volatile uint32_t *reg, uint32_t *ack_to_tim)
{
    if((NULL == reg) || (NULL == ack_to_tim))
        return false;

    *ack_to_tim = ((*reg) >> 0) & 0x1F;
    return true;
}

bool sysctl_pwr_get_tim(sysctl_pwr_domain_e powerdomain, sysctl_pwr_tim_e timtype, uint32_t *tim_value)
{
    volatile uint32_t *pwr_reg = NULL;
    volatile uint32_t *lpi_reg = NULL;
    volatile uint32_t *wfi_reg = NULL;

    if((powerdomain >= SYSCTL_PD_MAX) || (timtype >= SYSCTL_PWR_MAX_TIM))
        return false;

    if (!sysctl_pwr_get_timing_regs(powerdomain, &pwr_reg, &lpi_reg, &wfi_reg))
        return false;

    switch(timtype)
    {
        case SYSCTL_PWR_WFI_TIM:
            return sysctl_pwr_get_wfi_tim(wfi_reg, tim_value);
        case SYSCTL_PWR_SU_TIM:
            return sysctl_pwr_get_pwr_su_tim(pwr_reg, tim_value);
        case SYSCTL_PWR_PD_HD_TIM:
            return sysctl_pwr_get_pd_hd_tim(pwr_reg, tim_value);
        case SYSCTL_PWR_ISO_SU_TIM:
            return sysctl_pwr_get_iso_su_tim(pwr_reg, tim_value);
        case SYSCTL_PWR_IDLE_HD_TIM:
            return sysctl_pwr_get_idle_hd_tim(lpi_reg, tim_value);
        case SYSCTL_PWR_IDLE_TO_TIM:
            return sysctl_pwr_get_idle_to_tim(lpi_reg, tim_value);
        case SYSCTL_PWR_ACK_TO_TIM:
            return sysctl_pwr_get_ack_to_tim(lpi_reg, tim_value);
        default:
            return false;
    }
}


/*****************************************************************************************
*                                CPU1 KEEP RESET IN POWEROFF MODE
*   powerdomain: power domain
*   enable: poweron, keep reset; poweroff, remove reset
*****************************************************************************************/

/* 不工作的时候不会下电，可以处于被复位状态 */
bool sysctl_pwr_set_poweroff_keep_reset(sysctl_pwr_domain_e powerdomain, bool enable)
{
    volatile uint32_t *reg;

    if (powerdomain == SYSCTL_PD_CPU0)
        reg = (volatile uint32_t *)&sysctl_pwr->cpu0_pwr_lpi_ctl;
    else if (powerdomain == SYSCTL_PD_CPU1)
        reg = (volatile uint32_t *)&sysctl_pwr->cpu1_pwr_lpi_ctl;
    else
        return false;

    if (enable)
        *reg |= 1U << 3;
    else
        *reg &= ~(1U << 3);
    return true;
}

bool sysctl_pwr_get_poweroff_keep_reset(sysctl_pwr_domain_e powerdomain, bool *enable)
{
    if(SYSCTL_PD_CPU0 == powerdomain)
    {
        if(sysctl_pwr->cpu0_pwr_lpi_ctl & (1 << 3))
            *enable = true;
        else
            *enable = false;
        return true;
    } else
    if(SYSCTL_PD_CPU1 == powerdomain)
    {
        if(sysctl_pwr->cpu1_pwr_lpi_ctl & (1 << 3))
            *enable = true;
        else
            *enable = false;
        return true;
    }
    else
    {
        return false;
    }
}

/*****************************************************************************************
*                                CPU1 AUTO POWERUP OR POWERDOWN
*   powerdomain: power domain
*   enable: poweron, enable power control unit auto control mode; poweroff, disable auto
*****************************************************************************************/

/* MAIX3中，CPU0和CPU1电源域支持自动上下电管理 */
bool sysctl_pwr_set_auto_pwr(sysctl_pwr_domain_e powerdomain, bool enable)
{
    volatile uint32_t *reg;

    if (powerdomain == SYSCTL_PD_CPU0)
        reg = (volatile uint32_t *)&sysctl_pwr->cpu0_pwr_lpi_ctl;
    else if (powerdomain == SYSCTL_PD_CPU1)
        reg = (volatile uint32_t *)&sysctl_pwr->cpu1_pwr_lpi_ctl;
    else
        return false;

    if (enable)
        *reg |= 1U << 2;
    else
        *reg &= ~(1U << 2);
    return true;
}

bool sysctl_pwr_get_auto_pwr(sysctl_pwr_domain_e powerdomain, bool *enable)
{
    if(SYSCTL_PD_CPU0 == powerdomain)
    {
        if(sysctl_pwr->cpu0_pwr_lpi_ctl & (1 << 2))
            *enable = true;
        else
            *enable = false;
        return true;
    } else
    if(SYSCTL_PD_CPU1 == powerdomain)
    {
        if(sysctl_pwr->cpu1_pwr_lpi_ctl & (1 << 2))
            *enable = true;
        else
            *enable = false;
        return true;
    }
    else
    {
        return false;
    }
}

/*****************************************************************************************
*                                POWER DOMAIN REPAIR
*   powerdomain: power domain
*****************************************************************************************/

/* powerup的时候，设置电源域repair */
bool sysctl_pwr_set_repair_enable(sysctl_pwr_domain_e powerdomain)
{
    switch(powerdomain)
    {
        case SYSCTL_PD_AI:
            /* AI_PWR_LPI_CTL is write-enable protected: WE[20] guards bit 4. */
            sysctl_pwr->ai_pwr_lpi_ctl = (1U << 20) | (1U << 4);
            return true;
        case SYSCTL_PD_SHRM:
            /* SHRM_PWR_LPI_CTL is write-enable protected as well. */
            sysctl_pwr->shrm_pwr_lpi_ctl = (1U << 20) | (1U << 4);
            return true;
        default:
            return false;
    }
}

bool sysctl_pwr_check_repair_done(sysctl_pwr_domain_e powerdomain)
{
    switch(powerdomain)
    {
        case SYSCTL_PD_AI:
            return (sysctl_pwr->repair_status & (1 << 1)) ? true:false;
        case SYSCTL_PD_SHRM:
            return (sysctl_pwr->repair_status & (1U << 0)) ? true : false;
        default:
            return false;
    }
}

/*****************************************************************************************
*                                NOC POWER CONTROLLER
*   powerdomain: power domain
*   enable: true, connect noc, exit idle mode; false, disconnect noc, go idle mode.
*****************************************************************************************/

/* 设置不同power domain断开/连接NOC并且进入/离开idle状态 */
static bool sysctl_pwr_set_lpi_reg(volatile uint32_t *regctl,
    volatile uint32_t *regsta, uint32_t request_mask, uint32_t state_mask,
    bool write_enable)
{
    int i;

    if (regctl == NULL || regsta == NULL)
        return false;

    if ((*regsta & state_mask) == state_mask)
        return true;

    /*
     * Most media-domain PWR_LPI_CTL registers use the K230 write-enable
     * convention: WE[n + 16] must accompany request bit n.  Registers which
     * the TRM marks Write_enable=no must retain their other control fields.
     */
    if (write_enable)
        *regctl = request_mask | (request_mask << 16U);
    else
        *regctl |= request_mask;
    rt_hw_dsb();

    for (i = 0; i < SYSCTL_PWR_TRANSITION_TIMEOUT_US; i++) {
        if ((*regsta & state_mask) == state_mask)
            return true;
        cpu_ticks_delay_us(1);
    }

    return false;
}

static bool sysctl_pwr_ctl_has_write_enable(sysctl_pwr_domain_e powerdomain)
{
    return powerdomain != SYSCTL_PD_MCTL &&
           powerdomain != SYSCTL_PD_SEC &&
           powerdomain != SYSCTL_PD_ISP &&
           powerdomain != SYSCTL_PD_PMU;
}

bool sysctl_pwr_set_lpi(sysctl_pwr_domain_e powerdomain, bool enable)
{
    volatile uint32_t *regctl = NULL;
    volatile uint32_t *regsta = NULL;
    unsigned int exit_bit = 3;
    unsigned int go_bit = 2;
    unsigned int state_bit;
    uint32_t request_mask;
    uint32_t state_mask;
    bool write_enable = sysctl_pwr_ctl_has_write_enable(powerdomain);

    switch (powerdomain) {
    case SYSCTL_PD_CPU0:
        regctl = (volatile uint32_t *)&sysctl_pwr->cpu0_pwr_lpi_ctl;
        regsta = (volatile uint32_t *)&sysctl_pwr->cpu0_pwr_lpi_state;
        exit_bit = 5;
        go_bit = 4;
        break;
    case SYSCTL_PD_CPU1:
        regctl = (volatile uint32_t *)&sysctl_pwr->cpu1_pwr_lpi_ctl;
        regsta = (volatile uint32_t *)&sysctl_pwr->cpu1_pwr_lpi_state;
        exit_bit = 5;
        go_bit = 4;
        break;
    case SYSCTL_PD_AI:
        regctl = (volatile uint32_t *)&sysctl_pwr->ai_pwr_lpi_ctl;
        regsta = (volatile uint32_t *)&sysctl_pwr->ai_pwr_lpi_state;
        break;
    case SYSCTL_PD_DISP:
        regctl = (volatile uint32_t *)&sysctl_pwr->disp_lpi_ctl;
        regsta = (volatile uint32_t *)&sysctl_pwr->disp_lpi_state;
        break;
    case SYSCTL_PD_SHRM:
        regctl = (volatile uint32_t *)&sysctl_pwr->shrm_pwr_lpi_ctl;
        regsta = (volatile uint32_t *)&sysctl_pwr->shrm_pwr_lpi_state;
        break;
    case SYSCTL_PD_VPU:
        /*
         * The runtime driver quiesces the VPU NoC path here. The qchannel
         * remains QACTIVE until the following PCU power transition, so waiting
         * for qch_state_idle at this stage deadlocks in state 0x26.
         */
        regctl = (volatile uint32_t *)&sysctl_pwr->vpu_pwr_lpi_ctl;
        regsta = (volatile uint32_t *)&sysctl_pwr->vpu_lpi_state;
        break;
    case SYSCTL_PD_MCTL:
        regctl = (volatile uint32_t *)&sysctl_pwr->mctl_pwr_lpi_ctl;
        regsta = (volatile uint32_t *)&sysctl_pwr->mctl_lpi_state;
        break;
    case SYSCTL_PD_DPU:
        regctl = (volatile uint32_t *)&sysctl_pwr->dpu_pwr_lpi_ctl;
        regsta = (volatile uint32_t *)&sysctl_pwr->dpu_pwr_lpi_state;
        break;
    case SYSCTL_PD_HI:
        regctl = (volatile uint32_t *)&sysctl_pwr->hi_pwr_lpi_ctl;
        regsta = (volatile uint32_t *)&sysctl_pwr->hi_lpi_state;
        break;
    case SYSCTL_PD_LS:
        regctl = (volatile uint32_t *)&sysctl_pwr->ls_pwr_lpi_ctl;
        regsta = (volatile uint32_t *)&sysctl_pwr->ls_lpi_state;
        break;
    case SYSCTL_PD_SEC:
        regctl = (volatile uint32_t *)&sysctl_pwr->sec_pwr_lpi_ctl;
        regsta = (volatile uint32_t *)&sysctl_pwr->sec_pwr_lpi_state;
        break;
    case SYSCTL_PD_ISP:
        regctl = (volatile uint32_t *)&sysctl_pwr->isp_pwr_lpi_ctl;
        regsta = (volatile uint32_t *)&sysctl_pwr->isp_pwr_lpi_state;
        break;
    case SYSCTL_PD_PMU:
        regctl = (volatile uint32_t *)&sysctl_pwr->pmu_pwr_lpi_ctl;
        regsta = (volatile uint32_t *)&sysctl_pwr->pmu_pwr_lpi_state;
        break;
    default:
        return false;
    }

    state_bit = powerdomain == SYSCTL_PD_MCTL ?
        (enable ? 6U : 5U) : (enable ? 3U : 2U);
    request_mask = 1U << (enable ? exit_bit : go_bit);
    state_mask = 1U << state_bit;

    if (!sysctl_pwr_set_lpi_reg(regctl, regsta, request_mask, state_mask,
        write_enable)) {
        rt_kprintf("sysctl_pwr: lpi timeout pd=%d enable=%d req=0x%08x "
                   "want=0x%08x ctl=0x%08x state=0x%08x\n",
                   powerdomain, enable, request_mask, state_mask,
                   *regctl, *regsta);
        return false;
    }

    return true;
}

/*****************************************************************************************
*                                POWER DOMAIN ON OR OFF
*   powerdomain: power domain
*   enable: true for powerup, false for poweroff.
*****************************************************************************************/

static bool sysctl_pwr_set_mctl_reg(volatile uint32_t *regctl,
    volatile uint32_t *regsta, bool enable)
{
    const uint32_t mctl_power_off = 0x6U;

    if ((regctl == NULL) || (regsta == NULL))
        return false;

    /* MCTL_LPI_STAT uses the documented 5-bit power-controller state
     * encoding; 0x6 is PowerOff, not the generic low-bit status pair. */
    if (enable ? ((*regsta & 0x1fU) != mctl_power_off) :
        ((*regsta & 0x1fU) == mctl_power_off))
        return true;

    *regctl = (*regctl & ~0x3U) | (enable ? 0x2U : 0x1U);

    for (int i = 0; i < 100; i++) {
        if (enable ? ((*regsta & 0x1fU) != mctl_power_off) :
            ((*regsta & 0x1fU) == mctl_power_off))
            return true;
        for (int j = 0; j < 5000; j++);
    }

    return false;
}

static bool sysctl_pwr_set_pwr_reg_internal(volatile uint32_t *regctl,
    volatile uint32_t *regsta, bool enable, bool write_enable)
{
    uint32_t mask;

    if ((regctl == NULL) || (regsta == NULL))
        return false;

    mask = enable ? 0x2U : 0x1U;
    if ((*regsta & 0x3U) == mask)
        return true;

    if (write_enable)
        *regctl = (0x3U << 16U) | mask;
    else
        *regctl |= mask;
    rt_hw_dsb();

    for (int i = 0; i < SYSCTL_PWR_TRANSITION_TIMEOUT_US; i++) {
        if ((*regsta & 0x3U) == mask)
            return true;
        cpu_ticks_delay_us(1);
    }

    return false;
}

bool sysctl_pwr_set_pwr_reg(volatile uint32_t *regctl,
    volatile uint32_t *regsta, bool enable)
{
    return sysctl_pwr_set_pwr_reg_internal(regctl, regsta, enable, true);
}

bool sysctl_pwr_set_power(sysctl_pwr_domain_e powerdomain, bool enable)
{
    volatile uint32_t *pwr_ctl_reg = NULL;
    volatile uint32_t *pwr_sta_reg = NULL;
    bool ret;

    switch (powerdomain) {
    case SYSCTL_PD_CPU0:
        pwr_ctl_reg = (volatile uint32_t *)&sysctl_pwr->cpu0_pwr_lpi_ctl;
        pwr_sta_reg = (volatile uint32_t *)&sysctl_pwr->cpu0_pwr_lpi_state;
        break;
    case SYSCTL_PD_CPU1:
        pwr_ctl_reg = (volatile uint32_t *)&sysctl_pwr->cpu1_pwr_lpi_ctl;
        pwr_sta_reg = (volatile uint32_t *)&sysctl_pwr->cpu1_pwr_lpi_state;
        break;
    case SYSCTL_PD_AI:
        pwr_ctl_reg = (volatile uint32_t *)&sysctl_pwr->ai_pwr_lpi_ctl;
        pwr_sta_reg = (volatile uint32_t *)&sysctl_pwr->ai_pwr_lpi_state;
        break;
    case SYSCTL_PD_DISP:
        pwr_ctl_reg = (volatile uint32_t *)&sysctl_pwr->disp_lpi_ctl;
        pwr_sta_reg = (volatile uint32_t *)&sysctl_pwr->disp_lpi_state;
        break;
    case SYSCTL_PD_SHRM:
        pwr_ctl_reg = (volatile uint32_t *)&sysctl_pwr->shrm_pwr_lpi_ctl;
        pwr_sta_reg = (volatile uint32_t *)&sysctl_pwr->shrm_pwr_lpi_state;
        break;
    case SYSCTL_PD_VPU:
        pwr_ctl_reg = (volatile uint32_t *)&sysctl_pwr->vpu_pwr_lpi_ctl;
        pwr_sta_reg = (volatile uint32_t *)&sysctl_pwr->vpu_lpi_state;
        break;
    case SYSCTL_PD_MCTL:
        pwr_ctl_reg = (volatile uint32_t *)&sysctl_pwr->mctl_pwr_lpi_ctl;
        pwr_sta_reg = (volatile uint32_t *)&sysctl_pwr->mctl_lpi_state;
        break;
    case SYSCTL_PD_DPU:
        pwr_ctl_reg = (volatile uint32_t *)&sysctl_pwr->dpu_pwr_lpi_ctl;
        pwr_sta_reg = (volatile uint32_t *)&sysctl_pwr->dpu_pwr_lpi_state;
        break;
    case SYSCTL_PD_HI:
        pwr_ctl_reg = (volatile uint32_t *)&sysctl_pwr->hi_pwr_lpi_ctl;
        pwr_sta_reg = (volatile uint32_t *)&sysctl_pwr->hi_lpi_state;
        break;
    case SYSCTL_PD_LS:
        pwr_ctl_reg = (volatile uint32_t *)&sysctl_pwr->ls_pwr_lpi_ctl;
        pwr_sta_reg = (volatile uint32_t *)&sysctl_pwr->ls_lpi_state;
        break;
    case SYSCTL_PD_SEC:
        pwr_ctl_reg = (volatile uint32_t *)&sysctl_pwr->sec_pwr_lpi_ctl;
        pwr_sta_reg = (volatile uint32_t *)&sysctl_pwr->sec_pwr_lpi_state;
        break;
    case SYSCTL_PD_ISP:
        pwr_ctl_reg = (volatile uint32_t *)&sysctl_pwr->isp_pwr_lpi_ctl;
        pwr_sta_reg = (volatile uint32_t *)&sysctl_pwr->isp_pwr_lpi_state;
        break;
    case SYSCTL_PD_PMU:
        pwr_ctl_reg = (volatile uint32_t *)&sysctl_pwr->pmu_pwr_lpi_ctl;
        pwr_sta_reg = (volatile uint32_t *)&sysctl_pwr->pmu_pwr_lpi_state;
        break;
    default:
        return false;
    }

    if (enable)
        (void)sysctl_pwr_set_repair_enable(powerdomain);

    if (powerdomain == SYSCTL_PD_MCTL)
        return sysctl_pwr_set_mctl_reg(pwr_ctl_reg, pwr_sta_reg, enable);

    ret = sysctl_pwr_set_pwr_reg_internal(pwr_ctl_reg, pwr_sta_reg, enable,
        sysctl_pwr_ctl_has_write_enable(powerdomain));
    if (!ret) {
        rt_kprintf("sysctl_pwr: power timeout pd=%d enable=%d "
                   "ctl=0x%08x state=0x%08x\n",
                   powerdomain, enable, *pwr_ctl_reg, *pwr_sta_reg);
    }

    return ret;
}

bool sysctl_pwr_set_power_multi(sysctl_pwr_domain_e powerdomain, bool enable)
{
    int ret;

    if (enable)
        ret = k230_pd_get(powerdomain);
    else
        ret = k230_pd_put(powerdomain);

    if (ret == -K230_PM_ECTX)
        return sysctl_pwr_set_power(powerdomain, enable);
    if (!enable && ret == -K230_PM_EUNDERFLOW)
        return true;

    return ret == 0;
}

/* 电源域上电 */
bool sysctl_pwr_up(sysctl_pwr_domain_e powerdomain)
{
    return sysctl_pwr_set_power_multi(powerdomain, true);
}

/* 电源域下电 */
bool sysctl_pwr_off(sysctl_pwr_domain_e powerdomain)
{
    return sysctl_pwr_set_power_multi(powerdomain, false);
}

int rt_hw_sysctl_pwr_init(void)
{
    sysctl_pwr = rt_ioremap((void*)PWR_BASE_ADDR, PWR_IO_SIZE);
    if(!sysctl_pwr) {
        rt_kprintf("sysctl_pwr ioremap error\n");
        return -1;
    }

    return 0;
}
INIT_BOARD_EXPORT(rt_hw_sysctl_pwr_init);
