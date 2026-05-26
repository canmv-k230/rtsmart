/*
 * Copyright (c) 2006-2021, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2021-10-19     JasonHu      first version
 * 2021-11-12     JasonHu      fix bug that not intr on f133
 */

#include <rtthread.h>

#include <rtdbg.h>

#include "plic.h"
#include "rt_interrupt.h"
#include "io.h"
#include "encoding.h"
#include "ioremap.h"

static void *c908_plic_regs = RT_NULL;

struct plic_handler
{
    rt_bool_t present;
    void *hart_base;
    void *enable_base;
    void *prio;
    void *ip;
    void *sie;
    void *sth;
    void *sclaim;
};

rt_inline void plic_toggle(struct plic_handler *handler, int hwirq, int enable);
struct plic_handler c908_plic_handlers[C908_NR_CPUS];

rt_inline void *plic_priority_reg(struct plic_handler *handler, int hwirq)
{
    return handler->prio + PRIORITY_BASE + hwirq * PRIORITY_PER_ID;
}

void plic_set_priority(int irqno, rt_uint32_t priority)
{
    int cpu = 0;
    struct plic_handler *handler = &c908_plic_handlers[cpu];

    if ((irqno <= 0) || (irqno >= IRQ_MAX_NR))
    {
        return;
    }

    if ((c908_plic_regs == RT_NULL) || !handler->present)
    {
        return;
    }

    writel(priority, plic_priority_reg(handler, irqno));
}

void plic_complete(int irqno)
{
    int cpu = 0;
    struct plic_handler *handler = &c908_plic_handlers[cpu];

    writel(irqno, handler->sclaim);
}

void plic_disable_irq(int irqno)
{
    int cpu = 0;
    struct plic_handler *handler = &c908_plic_handlers[cpu];

    if ((irqno <= 0) || (irqno >= IRQ_MAX_NR))
    {
        return;
    }

    if (!handler->present)
    {
        return;
    }

    writel(0, plic_priority_reg(handler, irqno));
    plic_toggle(handler, irqno, 0);
}

void plic_enable_irq(int irqno)
{
    int cpu = 0;
    struct plic_handler *handler = &c908_plic_handlers[cpu];
    void *prio_reg;

    if ((irqno <= 0) || (irqno >= IRQ_MAX_NR))
    {
        return;
    }

    if (!handler->present)
    {
        return;
    }

    prio_reg = plic_priority_reg(handler, irqno);
    if (readl(prio_reg) == 0)
    {
        writel(1, prio_reg);
    }

    plic_toggle(handler, irqno, 1);
}

rt_err_t plic_get_irq_status(int irqno, struct plic_irq_status *status)
{
    int cpu = 0;
    struct plic_handler *handler = &c908_plic_handlers[cpu];
    uint32_t *enable_reg;
    uint32_t *pending_reg;
    uint32_t irq_mask;

    if (status == RT_NULL)
    {
        return -RT_EINVAL;
    }

    rt_memset(status, 0, sizeof(*status));

    if ((irqno <= 0) || (irqno >= IRQ_MAX_NR))
    {
        return -RT_EINVAL;
    }

    if ((c908_plic_regs == RT_NULL) || !handler->present)
    {
        return -RT_ERROR;
    }

    irq_mask = 1U << (irqno % 32);
    enable_reg = handler->sie + (irqno / 32) * sizeof(uint32_t);
    if (handler->ip == RT_NULL)
    {
        return -RT_ERROR;
    }

    pending_reg = handler->ip + (irqno / 32) * sizeof(uint32_t);

    status->present = handler->present;
    status->enabled = (readl(enable_reg) & irq_mask) ? RT_TRUE : RT_FALSE;
    status->pending = (readl(pending_reg) & irq_mask) ? RT_TRUE : RT_FALSE;
    status->priority = readl(handler->prio + PRIORITY_BASE + irqno * PRIORITY_PER_ID);
    status->threshold = readl(handler->sth);

    return RT_EOK;
}

/*
 * Handling an interrupt is a two-step process: first you claim the interrupt
 * by reading the claim register, then you complete the interrupt by writing
 * that source ID back to the same claim register.  This automatically enables
 * and disables the interrupt, so there's nothing else to do.
 */
void plic_handle_irq(void)
{
    int cpu = 0;
    unsigned int irq;

    struct plic_handler *handler = &c908_plic_handlers[cpu];
    void *claim = handler->sclaim;

    if (c908_plic_regs == RT_NULL || !handler->present)
    {
        LOG_E("plic state not initialized.");
        return;
    }

    clear_csr(sie, SIE_SEIE);

    while ((irq = readl(claim)))
    {
        /* ID0 is diabled permantually from spec. */
        if (irq == 0)
        {
            LOG_E("irq no is zero.");
        }
        else
        {
            generic_handle_irq(irq);
        }
    }
    set_csr(sie, SIE_SEIE);
}

rt_inline void plic_toggle(struct plic_handler *handler, int hwirq, int enable)
{
    uint32_t  *reg = handler->sie + (hwirq / 32) * sizeof(uint32_t);
    uint32_t hwirq_mask = 1 << (hwirq % 32);

    if (enable)
    {
        writel(readl(reg) | hwirq_mask, reg);
    }
    else
    {
        writel(readl(reg) & ~hwirq_mask, reg);
    }
}

void plic_init(void)
{
    int nr_irqs;
    int nr_context;
    int i;
    unsigned long hwirq;
    int cpu = 0;

    if (c908_plic_regs)
    {
        LOG_E("plic already initialized!");
        return;
    }

    nr_context = C908_NR_CONTEXT;

    c908_plic_regs = (void *)C908_PLIC_PHY_ADDR;

    if (!c908_plic_regs)
    {
        LOG_E("fatal error, plic is reg space is null.");
        return;
    }

    nr_irqs = C908_PLIC_NR_EXT_IRQS;

    for (i = 0; i < nr_context; i ++)
    {
        struct plic_handler *handler;
        uint32_t threshold = 0;

        cpu = 0;

        /* skip contexts other than supervisor external interrupt */
        if (i == 0)
        {
            continue;
        }

        // we always use CPU0 S-mode target register.
        handler = &c908_plic_handlers[cpu];
        if (handler->present)
        {
            threshold  = 0xffffffff;
            goto done;
        }

        handler->present = RT_TRUE;
        handler->hart_base = c908_plic_regs + CONTEXT_BASE + i * CONTEXT_PER_HART;
        handler->enable_base = c908_plic_regs + ENABLE_BASE + i * ENABLE_PER_HART;
        handler->prio = rt_ioremap_nocache(c908_plic_regs + PRIORITY_BASE, PRIORITY_PER_ID + nr_irqs * PRIORITY_PER_ID);
        handler->ip = rt_ioremap_nocache(c908_plic_regs + PENDING_BASE, (nr_irqs + 31) / 32 * 4);
        handler->sie = rt_ioremap_nocache(handler->enable_base, (nr_irqs + 31) / 32 * 4);
        handler->sth = rt_ioremap_nocache(handler->hart_base + CONTEXT_THRESHOLD, 4);
        handler->sclaim = rt_ioremap_nocache(handler->hart_base + CONTEXT_CLAIM, 4);
done:
        /* priority must be > threshold to trigger an interrupt */
        writel(threshold, handler->sth);

        for (hwirq = 1; hwirq <= nr_irqs; hwirq++)
        {
            plic_toggle(handler, hwirq, 0);
        }
    }

    /* Enable supervisor external interrupts. */
    set_csr(sie, SIE_SEIE);
}
