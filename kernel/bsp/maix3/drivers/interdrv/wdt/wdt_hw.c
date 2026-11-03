/* Copyright (c) 2026, Canaan Bright Sight Co., Ltd
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "wdt_hw.h"

#include <rtthread.h>
#include "board.h"
#include "ioremap.h"
#include <riscv_io.h>

#define WDT_REG_CR              0x00U
#define WDT_REG_TORR            0x04U
#define WDT_REG_CCVR            0x08U
#define WDT_REG_CRR             0x0CU
#define WDT_REG_STAT            0x10U
#define WDT_REG_EOI             0x14U
#define WDT_REG_COMP_PARAM_1    0xF4U

#define WDT_CR_EN               (1U << 0)
#define WDT_CR_RMOD             (1U << 1)
#define WDT_TORR_TOP_MASK       0x0FU
#define WDT_TORR_TOP_INIT_SHIFT 4
#define WDT_CRR_RESTART_KEY     0x76U
#define WDT_STAT_IRQ            (1U << 0)
#define WDT_COMP_PARAM_1_USE_FIX_TOP (1U << 6)

static void *wdt_base;

int wdt_hw_init(void)
{
    if (!wdt_base)
        wdt_base = rt_ioremap((void *)WDT1_BASE_ADDR, WDT1_IO_SIZE);
    return wdt_base ? RT_EOK : -RT_ENOMEM;
}

void wdt_hw_deinit(void)
{
    void *base = wdt_base;

    wdt_base = RT_NULL;
    if (base)
        rt_iounmap(base);
}

bool wdt_hw_ready(void)
{
    return wdt_base != RT_NULL;
}

static uint32_t wdt_readl(uint32_t offset)
{
    return readl((uint8_t *)wdt_base + offset);
}

static void wdt_writel(uint32_t value, uint32_t offset)
{
    writel(value, (uint8_t *)wdt_base + offset);
}

bool wdt_hw_enabled(void)
{
    return wdt_base && (wdt_readl(WDT_REG_CR) & WDT_CR_EN);
}

void wdt_hw_restart(void)
{
    if (wdt_base)
        wdt_writel(WDT_CRR_RESTART_KEY, WDT_REG_CRR);
}

void wdt_hw_enable(void)
{
    if (!wdt_base)
        return;
    wdt_hw_restart();
    wdt_writel(wdt_readl(WDT_REG_CR) | WDT_CR_EN, WDT_REG_CR);
}

void wdt_hw_set_response_mode(bool irq_mode)
{
    uint32_t ctrl;

    if (!wdt_base)
        return;
    ctrl = wdt_readl(WDT_REG_CR);
    ctrl = irq_mode ? (ctrl | WDT_CR_RMOD) : (ctrl & ~WDT_CR_RMOD);
    wdt_writel(ctrl, WDT_REG_CR);
}

/* With the WDT_DUAL_TOP option the first period after EN uses TOP_INIT
 * (bits 7:4), so program both as Linux dw_wdt does; the field is reserved
 * otherwise. */
void wdt_hw_set_top(uint32_t top)
{
    top &= WDT_TORR_TOP_MASK;
    if (wdt_base)
        wdt_writel(top | (top << WDT_TORR_TOP_INIT_SHIFT), WDT_REG_TORR);
}

uint32_t wdt_hw_get_top(void)
{
    return wdt_base ? wdt_readl(WDT_REG_TORR) & WDT_TORR_TOP_MASK : 0;
}

bool wdt_hw_uses_fix_top(void)
{
    return wdt_base && (wdt_readl(WDT_REG_COMP_PARAM_1) & WDT_COMP_PARAM_1_USE_FIX_TOP);
}

uint32_t wdt_hw_counter(void)
{
    return wdt_base ? wdt_readl(WDT_REG_CCVR) : 0;
}

bool wdt_hw_irq_pending(void)
{
    return wdt_base && (wdt_readl(WDT_REG_STAT) & WDT_STAT_IRQ);
}

/* Reading EOI clears the interrupt. */
void wdt_hw_clear_irq(void)
{
    if (wdt_base)
        (void)wdt_readl(WDT_REG_EOI);
}
